#include <stdint.h>
#include <vector>

#include "IR/Module.h"
#include "IR/Types.h"
#include "Inline/BasicTypes.h"
#include "Inline/Timing.h"
#include "LLVMEmitFunctionContext.h"
#include "LLVMEmitModuleContext.h"
#include "LLVMJITPrivate.h"

#include "LLVMPreInclude.h"

#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"

#include "LLVMPostInclude.h"

namespace llvm
{
	class Constant;
}

using namespace IR;
using namespace LLVMJIT;
using namespace Runtime;

EmitModuleContext::EmitModuleContext(const IR::Module& inIRModule,
									 LLVMContext& inLLVMContext,
									 llvm::Module* inLLVMModule)
: irModule(inIRModule)
, llvmContext(inLLVMContext)
, llvmModule(inLLVMModule)
, defaultMemoryOffset(nullptr)
, defaultTableOffset(nullptr)
, diBuilder(*inLLVMModule)
{
	diModuleScope = diBuilder.createFile("unknown", "unknown");
	diCompileUnit = diBuilder.createCompileUnit(0xffff, diModuleScope, "WAVM", true, "", 0);

	diValueTypes[(Uptr)ValueType::any] = nullptr;
	diValueTypes[(Uptr)ValueType::i32]
		= diBuilder.createBasicType("i32", 32, llvm::dwarf::DW_ATE_signed);
	diValueTypes[(Uptr)ValueType::i64]
		= diBuilder.createBasicType("i64", 64, llvm::dwarf::DW_ATE_signed);
	diValueTypes[(Uptr)ValueType::f32]
		= diBuilder.createBasicType("f32", 32, llvm::dwarf::DW_ATE_float);
	diValueTypes[(Uptr)ValueType::f64]
		= diBuilder.createBasicType("f64", 64, llvm::dwarf::DW_ATE_float);
	diValueTypes[(Uptr)ValueType::v128]
		= diBuilder.createBasicType("v128", 128, llvm::dwarf::DW_ATE_signed);
	diValueTypes[(Uptr)ValueType::anyref]
		= diBuilder.createBasicType("anyref", 8, llvm::dwarf::DW_ATE_address);
	diValueTypes[(Uptr)ValueType::anyfunc]
		= diBuilder.createBasicType("anyfunc", 8, llvm::dwarf::DW_ATE_address);
	diValueTypes[(Uptr)ValueType::nullref]
		= diBuilder.createBasicType("nullref", 8, llvm::dwarf::DW_ATE_address);

	auto zeroAsMetadata = llvm::ConstantAsMetadata::get(emitLiteral(llvmContext, I32(0)));
	auto i32MaxAsMetadata = llvm::ConstantAsMetadata::get(emitLiteral(llvmContext, I32(INT32_MAX)));
	likelyFalseBranchWeights = llvm::MDTuple::getDistinct(
		llvmContext,
		{llvm::MDString::get(llvmContext, "branch_weights"), zeroAsMetadata, i32MaxAsMetadata});
	likelyTrueBranchWeights = llvm::MDTuple::getDistinct(
		llvmContext,
		{llvm::MDString::get(llvmContext, "branch_weights"), i32MaxAsMetadata, zeroAsMetadata});

	fpRoundingModeMetadata = llvm::MetadataAsValue::get(
		llvmContext, llvm::MDString::get(llvmContext, "round.tonearest"));
	fpExceptionMetadata = llvm::MetadataAsValue::get(
		llvmContext, llvm::MDString::get(llvmContext, "fpexcept.strict"));

	tryPrologueDummyFunction = nullptr;
	cxaBeginCatchFunction = nullptr;
	if(!USE_WINDOWS_SEH)
	{
		cxaBeginCatchFunction = llvm::Function::Create(
			llvm::FunctionType::get(llvmContext.i8PtrType, {llvmContext.i8PtrType}, false),
			llvm::GlobalValue::LinkageTypes::ExternalLinkage,
			"__cxa_begin_catch",
			llvmModule);
	}
}

static llvm::Constant* createImportedConstant(llvm::Module& llvmModule, llvm::Twine externalName)
{
	return new llvm::GlobalVariable(llvmModule,
									llvm::Type::getInt8Ty(llvmModule.getContext()),
									false,
									llvm::GlobalVariable::ExternalLinkage,
									nullptr,
									externalName);
}

void LLVMJIT::emitModule(const IR::Module& irModule,
						 LLVMContext& llvmContext,
						 llvm::Module& outLLVMModule)
{
	Timing::Timer emitTimer;
	EmitModuleContext moduleContext(irModule, llvmContext, &outLLVMModule);

	// Create an external reference to the appropriate exception personality function.
	auto personalityFunction
		= llvm::Function::Create(llvm::FunctionType::get(llvmContext.i32Type, {}, false),
								 llvm::GlobalValue::LinkageTypes::ExternalLinkage,
#ifdef _WIN32
								 "__C_specific_handler",
#else
								 "__gxx_personality_v0",
#endif
								 &outLLVMModule);

	// Create LLVM external globals corresponding to the encoded function types for the module's
	// indexed function types.
	for(Uptr typeIndex = 0; typeIndex < irModule.types.size(); ++typeIndex)
	{
		moduleContext.typeIds.push_back(llvm::ConstantExpr::getPtrToInt(
			createImportedConstant(outLLVMModule, getExternalName("typeId", typeIndex)),
			llvmContext.iptrType));
	}

	// Create LLVM external globals corresponding to offsets to table base pointers in
	// CompartmentRuntimeData for the module's declared table objects.
	for(Uptr tableIndex = 0; tableIndex < irModule.tables.size(); ++tableIndex)
	{
		moduleContext.tableOffsets.push_back(llvm::ConstantExpr::getPtrToInt(
			createImportedConstant(outLLVMModule, getExternalName("tableOffset", tableIndex)),
			llvmContext.iptrType));
	}
	if(moduleContext.tableOffsets.size())
	{ moduleContext.defaultTableOffset = moduleContext.tableOffsets[0]; }

	// Create LLVM external globals corresponding to offsets to memory base pointers in
	// CompartmentRuntimeData for the module's declared memory objects.
	for(Uptr memoryIndex = 0; memoryIndex < irModule.memories.size(); ++memoryIndex)
	{
		moduleContext.memoryOffsets.push_back(llvm::ConstantExpr::getPtrToInt(
			createImportedConstant(outLLVMModule, getExternalName("memoryOffset", memoryIndex)),
			llvmContext.iptrType));
	}
	if(moduleContext.memoryOffsets.size())
	{ moduleContext.defaultMemoryOffset = moduleContext.memoryOffsets[0]; }

	// Create LLVM external globals for the module's globals.
	for(Uptr globalIndex = 0; globalIndex < irModule.globals.size(); ++globalIndex)
	{
		moduleContext.globals.push_back(
			createImportedConstant(outLLVMModule, getExternalName("global", globalIndex)));
	}

	// Create LLVM external globals corresponding to pointers to ExceptionTypeInstances for the
	// module's declared exception types.
	for(Uptr exceptionTypeIndex = 0; exceptionTypeIndex < irModule.exceptionTypes.size();
		++exceptionTypeIndex)
	{
		moduleContext.exceptionTypeInstances.push_back(createImportedConstant(
			outLLVMModule, getExternalName("exceptionType", exceptionTypeIndex)));
	}

	// Create a LLVM external global that will point to the ModuleInstance.
	moduleContext.moduleInstancePointer = createImportedConstant(outLLVMModule, "moduleInstance");

	// Create a LLVM external global that will be a bias applied to all references in a table.
	moduleContext.tableReferenceBias = llvm::ConstantExpr::getPtrToInt(
		createImportedConstant(outLLVMModule, "tableReferenceBias"), llvmContext.iptrType);

	// Create the LLVM functions.
	moduleContext.functions.resize(irModule.functions.size());
	for(Uptr functionIndex = 0; functionIndex < irModule.functions.size(); ++functionIndex)
	{
		FunctionType functionType = irModule.types[irModule.functions.getType(functionIndex).index];

		llvm::Function* function = llvm::Function::Create(
			asLLVMType(llvmContext, functionType, CallingConvention::wasm),
			llvm::Function::ExternalLinkage,
			functionIndex >= irModule.functions.imports.size()
				? getExternalName("functionDef", functionIndex - irModule.functions.imports.size())
				: getExternalName("functionImport", functionIndex),
			&outLLVMModule);
		function->setCallingConv(asLLVMCallingConv(CallingConvention::wasm));
		moduleContext.functions[functionIndex] = function;
	}

	// Compile each function in the module.
	for(Uptr functionDefIndex = 0; functionDefIndex < irModule.functions.defs.size();
		++functionDefIndex)
	{
		const FunctionDef& functionDef = irModule.functions.defs[functionDefIndex];
		llvm::Function* function
			= moduleContext.functions[irModule.functions.imports.size() + functionDefIndex];

		function->setPersonalityFn(personalityFunction);

		llvm::Constant* functionInstance = llvm::ConstantExpr::getPtrToInt(
			createImportedConstant(outLLVMModule,
								   getExternalName("functionDefInstance", functionDefIndex)),
			llvmContext.iptrType);

		function->setPrefixData(llvm::ConstantArray::get(
			llvm::ArrayType::get(llvmContext.iptrType, 2),
			{functionInstance, moduleContext.typeIds[functionDef.type.index]}));

		EmitFunctionContext(llvmContext, moduleContext, irModule, functionDef, function).emit();
	}

	// Finalize the debug info.
	moduleContext.diBuilder.finalize();

	Timing::logRatePerSecond("Emitted LLVM IR", emitTimer, (F64)outLLVMModule.size(), "functions");
}
