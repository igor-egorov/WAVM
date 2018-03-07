#include "Inline/BasicTypes.h"
#include "Logging/Logging.h"
#include "Intrinsics.h"
#include "RuntimePrivate.h"

#include <thread>
#include <vector>
#include <atomic>
#include <cmath>
#include <algorithm>

// Keeps track of the entry and error functions used by a running WebAssembly-spawned thread.
// Used to find garbage collection roots.
struct Thread
{
	Runtime::GCPointer<Runtime::Context> context;
	Runtime::GCPointer<Runtime::FunctionInstance> entryFunction;
	Runtime::GCPointer<Runtime::FunctionInstance> errorFunction;
};

// Holds a list of threads (in the form of events that will wake them) that
// are waiting on a specific address.
struct WaitList
{
	Platform::Mutex* mutex;
	std::vector<Platform::Event*> wakeEvents;
	std::atomic<Uptr> numReferences;

	WaitList(): mutex(Platform::createMutex()), numReferences(1) {}
	~WaitList() { destroyMutex(mutex); }
};

// An event that is reused within a thread when it waits on a WaitList.
THREAD_LOCAL Platform::Event* threadWakeEvent = nullptr;

// A map from address to a list of threads waiting on that address.
static Platform::Mutex* addressToWaitListMapMutex = Platform::createMutex();
static std::map<Uptr,WaitList*> addressToWaitListMap;

// A global list of running threads created by WebAssembly code.
static Platform::Mutex* threadsMutex = Platform::createMutex();
static std::vector<Thread*> threads;

// Opens the wait list for a given address.
// Increases the wait list's reference count, and returns a pointer to it.
// Note that it does not lock the wait list mutex.
// A call to openWaitList should be followed by a call to closeWaitList to avoid leaks.
static WaitList* openWaitList(Uptr address)
{
	Platform::Lock mapLock(addressToWaitListMapMutex);
	auto mapIt = addressToWaitListMap.find(address);
	if(mapIt != addressToWaitListMap.end())
	{
		++mapIt->second->numReferences;
		return mapIt->second;
	}
	else
	{
		WaitList* waitList = new WaitList();
		addressToWaitListMap.emplace(address,waitList);
		return waitList;
	}
}

// Closes a wait list, deleting it and removing it from the global map if it was the last reference.
static void closeWaitList(Uptr address,WaitList* waitList)
{
	if(--waitList->numReferences == 0)
	{
		Platform::Lock mapLock(addressToWaitListMapMutex);
		if(!waitList->numReferences)
		{
			assert(!waitList->wakeEvents.size());
			delete waitList;
			addressToWaitListMap.erase(address);
		}
	}
}

// Loads a value from memory with seq_cst memory order.
// The caller must ensure that the pointer is naturally aligned.
template<typename Value>
static Value atomicLoad(const Value* valuePointer)
{
	static_assert(sizeof(std::atomic<Value>) == sizeof(Value),"relying on non-standard behavior");
	std::atomic<Value>* valuePointerAtomic = (std::atomic<Value>*)valuePointer;
	return valuePointerAtomic->load();
}

// Stores a value to memory with seq_cst memory order.
// The caller must ensure that the pointer is naturally aligned.
template<typename Value>
static void atomicStore(Value* valuePointer,Value newValue)
{
	static_assert(sizeof(std::atomic<Value>) == sizeof(Value),"relying on non-standard behavior");
	std::atomic<Value>* valuePointerAtomic = (std::atomic<Value>*)valuePointer;
	valuePointerAtomic->store(newValue);
}

// Decodes a floating-point timeout value relative to startTime.
U64 getEndTimeFromTimeout(U64 startTime,F64 timeout)
{
	const F64 timeoutMicroseconds = timeout * 1000.0;
	U64 endTime = UINT64_MAX;
	if(!std::isnan(timeoutMicroseconds) && std::isfinite(timeoutMicroseconds))
	{
		if(timeoutMicroseconds <= 0.0)
		{
			endTime = startTime;
		}
		else if(timeoutMicroseconds <= F64(UINT64_MAX - 1))
		{
			endTime = startTime + U64(timeoutMicroseconds);
			errorUnless(endTime >= startTime);
		}
	}
	return endTime;
}

template<typename Value>
static U32 waitOnAddress(Value* valuePointer,Value expectedValue,F64 timeout)
{
	const U64 endTime = getEndTimeFromTimeout(Platform::getMonotonicClock(),timeout);

	// Open the wait list for this address.
	const Uptr address = reinterpret_cast<Uptr>(valuePointer);
	WaitList* waitList = openWaitList(address);

	// Lock the wait list, and check that *valuePointer is still what the caller expected it to be.
	lockMutex(waitList->mutex);
	if(atomicLoad(valuePointer) != expectedValue)
	{
		// If *valuePointer wasn't the expected value, unlock the wait list and return.
		unlockMutex(waitList->mutex);
		closeWaitList(address,waitList);
		return 1;
	}
	else
	{
		// If the thread hasn't yet created a wake event, do so.
		if(!threadWakeEvent) { threadWakeEvent = Platform::createEvent(); }

		// Add the wake event to the wait list, and unlock the wait list.
		waitList->wakeEvents.push_back(threadWakeEvent);
		unlockMutex(waitList->mutex);
	}

	// Wait for the thread's wake event to be signaled.
	bool timedOut = false;
	if(!Platform::waitForEvent(threadWakeEvent,endTime))
	{
		// If the wait timed out, lock the wait list and check if the thread's wake event is still in the wait list.
		Platform::Lock waitListLock(waitList->mutex);
		auto wakeEventIt = std::find(waitList->wakeEvents.begin(),waitList->wakeEvents.end(),threadWakeEvent);
		if(wakeEventIt != waitList->wakeEvents.end())
		{
			// If the event was still on the wait list, remove it, and return the "timed out" result.
			waitList->wakeEvents.erase(wakeEventIt);
			timedOut = true;
		}
		else
		{
			// In between the wait timing out and locking the wait list, some other thread tried to wake this thread.
			// The event will now be signaled, so use an immediately expiring wait on it to reset it.
			errorUnless(Platform::waitForEvent(threadWakeEvent,Platform::getMonotonicClock()));
		}
	}

	closeWaitList(address,waitList);
	return timedOut ? 2 : 0;
}

static U32 wakeAddress(Uptr address,U32 numToWake)
{
	if(numToWake == 0) { return 0; }

	// Open the wait list for this address.
	WaitList* waitList = openWaitList(address);
	Uptr actualNumToWake = numToWake;
	{
		Platform::Lock waitListLock(waitList->mutex);

		// Determine how many threads to wake.
		// numToWake==UINT32_MAX means wake all waiting threads.
		if(actualNumToWake == UINT32_MAX || actualNumToWake > waitList->wakeEvents.size())
		{
			actualNumToWake = waitList->wakeEvents.size();
		}

		// Signal the events corresponding to the oldest waiting threads.
		for(Uptr wakeIndex = 0;wakeIndex < actualNumToWake;++wakeIndex)
		{
			signalEvent(waitList->wakeEvents[wakeIndex]);
		}

		// Remove the events from the wait list.
		waitList->wakeEvents.erase(waitList->wakeEvents.begin(),waitList->wakeEvents.begin() + actualNumToWake);
	}
	closeWaitList(address,waitList);

	if(actualNumToWake > UINT32_MAX)
	{
		Runtime::throwException(Runtime::Exception::integerDivideByZeroOrIntegerOverflowType);
	}
	return U32(actualNumToWake);
}

namespace Runtime
{
	DEFINE_INTRINSIC_FUNCTION1(wavmIntrinsics,misalignedAtomicTrap,misalignedAtomicTrap,none,i32,address)
	{
		throwException(Exception::misalignedAtomicMemoryAccessType);
	}

	DEFINE_INTRINSIC_FUNCTION3(wavmIntrinsics,atomic_wake,atomic_wake,i32,i32,addressOffset,i32,numToWake,i64,memoryId)
	{
		MemoryInstance* memoryInstance = getMemoryFromRuntimeData(*_context,memoryId);

		// Validate that the address is within the memory's bounds and 4-byte aligned.
		if(U32(addressOffset) > memoryInstance->endOffset) { throwException(Exception::accessViolationType); }
		if(addressOffset & 3) { throwException(Exception::misalignedAtomicMemoryAccessType); }

		const Uptr address = reinterpret_cast<Uptr>(&memoryRef<U8>(memoryInstance,addressOffset));
		return wakeAddress(address,numToWake);
	}

	DEFINE_INTRINSIC_FUNCTION4(wavmIntrinsics,atomic_wait,atomic_wait_i32,i32,i32,addressOffset,i32,expectedValue,f64,timeout,i64,memoryId)
	{
		MemoryInstance* memoryInstance = getMemoryFromRuntimeData(*_context,memoryId);

		// Validate that the address is within the memory's bounds and naturally aligned.
		if(U32(addressOffset) > memoryInstance->endOffset) { throwException(Exception::accessViolationType); }
		if(addressOffset & 3) { throwException(Exception::misalignedAtomicMemoryAccessType); }

		I32* valuePointer = &memoryRef<I32>(memoryInstance,addressOffset);
		return waitOnAddress(valuePointer,expectedValue,timeout);
	}
	DEFINE_INTRINSIC_FUNCTION4(wavmIntrinsics,atomic_wait,atomic_wait_i64,i32,i32,addressOffset,i64,expectedValue,f64,timeout,i64,memoryId)
	{
		MemoryInstance* memoryInstance = getMemoryFromRuntimeData(*_context,memoryId);

		// Validate that the address is within the memory's bounds and naturally aligned.
		if(U32(addressOffset) > memoryInstance->endOffset) { throwException(Exception::accessViolationType); }
		if(addressOffset & 7) { throwException(Exception::misalignedAtomicMemoryAccessType); }

		I64* valuePointer = &memoryRef<I64>(memoryInstance,addressOffset);
		return waitOnAddress(valuePointer,expectedValue,timeout);
	}

	FunctionInstance* getFunctionFromTable(TableInstance* table,const FunctionType* expectedType,U32 elementIndex)
	{
		// Validate that the index is valid.
		if(elementIndex * sizeof(TableInstance::FunctionElement) >= table->endOffset)
		{
			throwException(Runtime::Exception::undefinedTableElementType);
		}
		// Validate  that the indexed function's type matches the expected type.
		const FunctionType* actualSignature = table->baseAddress[elementIndex].type;
		if(actualSignature != expectedType)
		{
			throwException(Runtime::Exception::indirectCallSignatureMismatchType);
		}
		return asFunction(table->elements[elementIndex]);
	}
	
	static void threadFunc(Thread* thread,I32 argument)
	{
		catchRuntimeExceptions(
			[&]
			{
				invokeFunction(thread->context,thread->entryFunction,{ argument });
			},
			[&](Exception&& exception)
			{
				// Log that a runtime exception was handled by a thread error function.
				Log::printf(Log::Category::error,"Runtime exception in thread: %s\n",describeException(exception).c_str());
				Log::printf(Log::Category::error,"Passing exception on to thread error handler\n");

				catchRuntimeExceptions(
					[&]
					{
						// Call the thread error function.
						invokeFunction(thread->context,thread->errorFunction,{argument});
					},
					[&](Exception&& secondException)
					{
						// Log that the thread error function caused a runtime exception, and exit with a fatal error.
						Log::printf(Log::Category::error,"Runtime exception in thread error handler: %s\n",describeException(secondException).c_str());
						Errors::fatalf("double fault");
					});
			});

		// Destroy the thread wake event before exiting the thread.
		if(threadWakeEvent)
		{
			Platform::destroyEvent(threadWakeEvent);
		}
		
		// Remove the thread from the global list.
		{
			Platform::Lock threadsLock(threadsMutex);
			auto threadIt = std::find(threads.begin(),threads.end(),thread);
			threads.erase(threadIt);
		}

		// Delete the thread object.
		delete thread;
	}
	
	DEFINE_INTRINSIC_FUNCTION4(wavmIntrinsics,launchThread,launchThread,none,
		i32,entryFunctionIndex,
		i32,argument,
		i32,errorFunctionIndex,
		i64,tableId)
	{
		TableInstance* defaultTable = getTableFromRuntimeData(*_context,tableId);
		const FunctionType* functionType = FunctionType::get(ResultType::none,{ValueType::i32});

		// Create a thread object that will expose its entry and error functions to the garbage collector as roots.
		Thread* thread = new Thread();
		thread->context = cloneContext(getContextFromRuntimeData(*_context));
		thread->entryFunction = getFunctionFromTable(defaultTable,functionType,entryFunctionIndex);
		thread->errorFunction = getFunctionFromTable(defaultTable,functionType,errorFunctionIndex);
		{
			Platform::Lock threadsLock(threadsMutex);
			threads.push_back(thread);
		}

		// Use a std::thread to spawn the thread.
		std::thread stdThread([thread,argument]()
		{
			threadFunc(thread,argument);
		});

		// Detach the std::thread from the underlying thread.
		stdThread.detach();
	}
}
