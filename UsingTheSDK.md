# Using the SDK

## 1. Adding the SDK to your project

1. Copy the generated `CppSDK` folder into your project.
2. Include `SDK.hpp` to get everything, or include single package headers
   (`SDK/Engine_classes.hpp`) for much faster compile times.
3. Compile `SDK/Basic.cpp`, plus every `SDK/<Package>_functions.cpp` whose functions you call.
4. Build with **C++20**. clang and gcc are both supported.

## 2. The accessors

There are five functions that connect the SDK to the running process. `SDK/Basic.cpp` has all five functions
implemented for the layout the dumper detected, so an unmodified SDK builds and runs as generated.

```c++
uintptr_t      InSDKUtils::GetImageBase();
uintptr_t      InSDKUtils::GetGNames();
TUObjectArray* InSDKUtils::GetGObjects();
std::string    InSDKUtils::GetNameByIndex(int32 Index);
UObject*       InSDKUtils::GetObjectByIndex(int32 Index);
```

### 2.1 What each default does

`GetImageBase()` locates the module that every `Offsets::` value is relative to, searching the
loaded images for `EngineInfo::UnrealModuleName` (`dl_iterate_phdr` on Android, the dyld image list
on iOS). What it returns differs by platform, because the dumper measures offsets differently on
each:

| Platform | Returned value | Consequence |
| --- | --- | --- |
| Android | load address of the UE library | `Offsets::` are module-relative |
| iOS | ASLR slide of the UE executable | `Offsets::` are un-slid virtual addresses |

The remaining four follow the layout recorded at dump time:

| Function | Generated behaviour |
| --- | --- |
| `GetGNames` | adds `Offsets::GNames` to the image base, dereferencing it when the dumper found a pointer rather than the container itself |
| `GetGObjects` | the same choice for `Offsets::GObjects` |
| `GetObjectByIndex` | indexes a fixed array directly, or selects a chunk first when the array is chunked |
| `GetNameByIndex` | decodes a legacy `TNameEntryArray` entry or an `FNamePool` entry |

A game that resolves these differently can be
accommodated by manually editing those functions.

### 2.2 When the defaults are not enough

The generated bodies read memory **as-is**. A game that obfuscates/encrypts any value on these paths needs
them adapted, because the dumper applies its decryption through custom profiles that have no equivalent in the generated SDK. The affected values are:

- the `GObjects` and `GNames` pointers themselves (`DecryptGObjects`, `DecryptGNames`)
- the object array's chunk table, counts, and the `UObject*` inside each `FUObjectItem`
- the name container's chunk or block table, and an `FNameEntry`'s header or packed index
- the name characters, which a profile may decrypt in place per encoding
- `FName`'s comparison index and number

A dump can succeed while the SDK reads nothing useful, since only the dumper side knows how to undo
the obfuscation. Porting the matching profile logic into the accessor is what closes that gap.

### 2.3 Overriding a default permanently

Setting `GetImageBaseFuncBody`, `GetGNamesFuncBody`, `GetGObjectsBody`, `GetNameByIndexFuncBody` or
`GetObjectByIndexFuncBody` in [Dumper/Settings.h](Dumper/Settings.h) replaces that body in every
SDK generated afterwards, which is the better option than re-editing a regenerated file. An empty
setting keeps the generated default.

## 3. Using the SDK

### 3.1 Engine build info

`EngineInfo` records what the SDK was generated against, so code can pin itself to that build
instead of failing obscurely against another one:

```c++
/* Fails the build rather than at runtime if the SDK is regenerated from a different engine. */
static_assert(SDK::EngineInfo::EngineMajor == 4 && SDK::EngineInfo::EngineMinor == 27,
              "Written against UE 4.27 - review before using an SDK from another build.");

/* Branch at compile time on features that moved between versions. */
if constexpr (SDK::EngineInfo::EngineMajor >= 5)
{
    // UE5-only paths
}

/* Needed when reading a TCHAR buffer yourself: 2 for char16_t, 4 for a 4-byte wchar_t. */
const SDK::int32 BytesPerChar = SDK::EngineInfo::TCharSize;
```

Logging it on startup makes an SDK/build mismatch obvious, which otherwise looks like bad offsets:

```c++
printf("SDK built for %s (%s), TCHAR=%d bytes\n",
       SDK::EngineInfo::EngineVersion,
       SDK::EngineInfo::UnrealModuleName,
       SDK::EngineInfo::TCharSize);
```

### 3.2 Retrieving instances

```c++
SDK::UObject* Obj1 = SDK::UObject::FindObject<SDK::UObject>("ClassName Package.Outer.ObjectName");
SDK::UObject* Obj2 = SDK::UObject::FindObjectFast<SDK::UObject>("ObjectName");

/* No offset required, these replace GWorld/GEngine */
SDK::UWorld*  World  = SDK::UWorld::GetWorld();
SDK::UEngine* Engine = SDK::UEngine::GetEngine();
```

### 3.3 Calling functions

```c++
/* Non-static functions need an instance */
float OutX, OutY;
MyController->GetMousePosition(&OutX, &OutY);

/* Static functions are called on their DefaultObject automatically */
SDK::FName Name = SDK::UKismetStringLibrary::Conv_StringToName(L"DemoNetDriver");
```

### 3.4 Checking a UObject's type

```c++
/* Fastest, but limited to base types */
const bool bIsActor = Obj->IsA(SDK::EClassCastFlags::Actor);

/* Ideal for native classes */
const bool bIsPawn = Obj->IsA(SDK::APawn::StaticClass());

/* Works for every class, including Blueprint classes */
const bool bIsBP = Obj->IsA(SDK::ABP_Pawn_C::StaticName());
```

### 3.5 Casting

Unreal relies heavily on inheritance, so base-class pointers often hold child instances. Check the
type before casting:

```c++
if (MyController->Pawn->IsA(SDK::AGameSpecificPawn::StaticClass()))
{
    SDK::AGameSpecificPawn* Pawn = static_cast<SDK::AGameSpecificPawn*>(MyController->Pawn);
    Pawn->GameSpecificVariable = 30;
}
```

### 3.6 Iterating GObjects

```c++
SDK::TUObjectArray* GObjects = SDK::InSDKUtils::GetGObjects();

for (SDK::int32 i = 0; i < GObjects->Num(); i++)
{
    SDK::UObject* Obj = SDK::InSDKUtils::GetObjectByIndex(i);

    if (!Obj || Obj->IsDefaultObject())
        continue;

    if (Obj->IsA(SDK::EClassCastFlags::Pawn))
        printf("%s\n", Obj->GetFullName().c_str());
}
```

## 4. Code

An injected library that walks the persistent level:

```c++
#include <pthread.h>
#include <unistd.h>

#include "SDK/Engine_classes.hpp"

// Basic.cpp and Engine_functions.cpp are compiled into this library

static void* MainThread(void*)
{
    /* Give the engine time to initialise before touching GObjects */
    sleep(20);

    /* Worth logging first: if the base is 0 the module was not found under
       EngineInfo::UnrealModuleName, and every offset below it would be meaningless. */
    printf("SDK built for %s, module %s, image base 0x%lx\n",
           SDK::EngineInfo::EngineVersion,
           SDK::EngineInfo::UnrealModuleName,
           (unsigned long)SDK::InSDKUtils::GetImageBase());

    SDK::UWorld* World = SDK::UWorld::GetWorld();
    if (!World || !World->PersistentLevel)
        return nullptr;

    /* Every pointer along a chain should be null-checked */
    SDK::APlayerController* Controller = World->OwningGameInstance->LocalPlayers[0]->PlayerController;

    SDK::TArray<SDK::AActor*>& Actors = World->PersistentLevel->Actors;

    for (SDK::AActor* Actor : Actors)
    {
        if (!Actor || !Actor->IsA(SDK::EClassCastFlags::Pawn))
            continue;

        SDK::APawn* Pawn = static_cast<SDK::APawn*>(Actor);
        // Use Pawn here
    }

    return nullptr;
}

__attribute__((constructor)) static void OnLoad()
{
    pthread_t Thread;
    pthread_create(&Thread, nullptr, MainThread, nullptr);
}
```

## Notes

- Everything lives in `namespace SDK`, configurable via `CppGenerator::SDKNamespaceName`.
- A failing `static_assert` from `Assertions.inl` means the dumped layout disagrees with the
  compiler, so memory reads/writes would land on the wrong bytes. The checks are controlled by
  `Debug::bGenerateAssertionFile` and the two `Debug::bGenerateInlineAssertionsFor*` flags in
  [Dumper/Settings.h](Dumper/Settings.h); with all three off, no assertions are emitted at all.
  Turning them off only stops the compiler reporting the disagreement, and the offsets may still be
  wrong. Use it to get a build moving rather than to resolve the failure.
- `EngineInfo` records what the dump was taken against: `EngineMajor`/`Minor`/`Patch`, the full
  `EngineVersion` string, `TCharSize` (2 for `char16_t`, 4 for a 4-byte `wchar_t`) and
  `UnrealModuleName`. Useful for gating code on an engine version, and for confirming an SDK
  belongs to the build you are attached to.
