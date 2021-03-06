//===---- OrcRemoteTargetServer.h - Orc Remote-target Server ----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the OrcRemoteTargetServer class. It can be used to build a
// JIT server that can execute code sent from an OrcRemoteTargetClient.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_ORCREMOTETARGETSERVER_H
#define LLVM_EXECUTIONENGINE_ORC_ORCREMOTETARGETSERVER_H

#include "OrcRemoteTargetRPCAPI.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"
#include <map>

#define DEBUG_TYPE "orc-remote"

namespace llvm {
namespace orc {
namespace remote {

template <typename ChannelT, typename TargetT>
class OrcRemoteTargetServer : public OrcRemoteTargetRPCAPI {
public:
  typedef std::function<TargetAddress(const std::string &Name)>
      SymbolLookupFtor;

  typedef std::function<void(uint8_t *Addr, uint32_t Size)>
      EHFrameRegistrationFtor;

  OrcRemoteTargetServer(ChannelT &Channel, SymbolLookupFtor SymbolLookup,
                        EHFrameRegistrationFtor EHFramesRegister,
                        EHFrameRegistrationFtor EHFramesDeregister)
      : Channel(Channel), SymbolLookup(std::move(SymbolLookup)),
        EHFramesRegister(std::move(EHFramesRegister)),
        EHFramesDeregister(std::move(EHFramesDeregister)) {}

  // FIXME: Remove move/copy ops once MSVC supports synthesizing move ops.
  OrcRemoteTargetServer(const OrcRemoteTargetServer&) = delete;
  OrcRemoteTargetServer& operator=(const OrcRemoteTargetServer&) = delete;

  OrcRemoteTargetServer(OrcRemoteTargetServer &&Other)
    : Channel(Other.Channel),
      SymbolLookup(std::move(Other.SymbolLookup)),
      EHFramesRegister(std::move(Other.EHFramesRegister)),
      EHFramesDeregister(std::move(Other.EHFramesDeregister)) {}

  OrcRemoteTargetServer& operator=(OrcRemoteTargetServer&&) = delete;

  std::error_code handleKnownFunction(JITFuncId Id) {
    typedef OrcRemoteTargetServer ThisT;

    DEBUG(dbgs() << "Handling known proc: " << getJITFuncIdName(Id) << "\n");

    switch (Id) {
    case CallIntVoidId:
      return handle<CallIntVoid>(Channel, *this, &ThisT::handleCallIntVoid);
    case CallMainId:
      return handle<CallMain>(Channel, *this, &ThisT::handleCallMain);
    case CallVoidVoidId:
      return handle<CallVoidVoid>(Channel, *this, &ThisT::handleCallVoidVoid);
    case CreateRemoteAllocatorId:
      return handle<CreateRemoteAllocator>(Channel, *this,
                                           &ThisT::handleCreateRemoteAllocator);
    case CreateIndirectStubsOwnerId:
      return handle<CreateIndirectStubsOwner>(
          Channel, *this, &ThisT::handleCreateIndirectStubsOwner);
    case DeregisterEHFramesId:
      return handle<DeregisterEHFrames>(Channel, *this,
                                        &ThisT::handleDeregisterEHFrames);
    case DestroyRemoteAllocatorId:
      return handle<DestroyRemoteAllocator>(
          Channel, *this, &ThisT::handleDestroyRemoteAllocator);
    case DestroyIndirectStubsOwnerId:
      return handle<DestroyIndirectStubsOwner>(
          Channel, *this, &ThisT::handleDestroyIndirectStubsOwner);
    case EmitIndirectStubsId:
      return handle<EmitIndirectStubs>(Channel, *this,
                                       &ThisT::handleEmitIndirectStubs);
    case EmitResolverBlockId:
      return handle<EmitResolverBlock>(Channel, *this,
                                       &ThisT::handleEmitResolverBlock);
    case EmitTrampolineBlockId:
      return handle<EmitTrampolineBlock>(Channel, *this,
                                         &ThisT::handleEmitTrampolineBlock);
    case GetSymbolAddressId:
      return handle<GetSymbolAddress>(Channel, *this,
                                      &ThisT::handleGetSymbolAddress);
    case GetRemoteInfoId:
      return handle<GetRemoteInfo>(Channel, *this, &ThisT::handleGetRemoteInfo);
    case ReadMemId:
      return handle<ReadMem>(Channel, *this, &ThisT::handleReadMem);
    case RegisterEHFramesId:
      return handle<RegisterEHFrames>(Channel, *this,
                                      &ThisT::handleRegisterEHFrames);
    case ReserveMemId:
      return handle<ReserveMem>(Channel, *this, &ThisT::handleReserveMem);
    case SetProtectionsId:
      return handle<SetProtections>(Channel, *this,
                                    &ThisT::handleSetProtections);
    case WriteMemId:
      return handle<WriteMem>(Channel, *this, &ThisT::handleWriteMem);
    case WritePtrId:
      return handle<WritePtr>(Channel, *this, &ThisT::handleWritePtr);
    default:
      return orcError(OrcErrorCode::UnexpectedRPCCall);
    }

    llvm_unreachable("Unhandled JIT RPC procedure Id.");
  }

  ErrorOr<TargetAddress> requestCompile(TargetAddress TrampolineAddr) {
    auto Listen =
      [&](RPCChannel &C, uint32_t Id) {
        return handleKnownFunction(static_cast<JITFuncId>(Id));
      };

    return callSTHandling<RequestCompile>(Channel, Listen, TrampolineAddr);
  }

  void handleTerminateSession() {
    handle<TerminateSession>(Channel, [](){ return std::error_code(); });
  }

private:
  struct Allocator {
    Allocator() = default;
    Allocator(Allocator &&Other) : Allocs(std::move(Other.Allocs)) {}
    Allocator &operator=(Allocator &&Other) {
      Allocs = std::move(Other.Allocs);
      return *this;
    }

    ~Allocator() {
      for (auto &Alloc : Allocs)
        sys::Memory::releaseMappedMemory(Alloc.second);
    }

    std::error_code allocate(void *&Addr, size_t Size, uint32_t Align) {
      std::error_code EC;
      sys::MemoryBlock MB = sys::Memory::allocateMappedMemory(
          Size, nullptr, sys::Memory::MF_READ | sys::Memory::MF_WRITE, EC);
      if (EC)
        return EC;

      Addr = MB.base();
      assert(Allocs.find(MB.base()) == Allocs.end() && "Duplicate alloc");
      Allocs[MB.base()] = std::move(MB);
      return std::error_code();
    }

    std::error_code setProtections(void *block, unsigned Flags) {
      auto I = Allocs.find(block);
      if (I == Allocs.end())
        return orcError(OrcErrorCode::RemoteMProtectAddrUnrecognized);
      return sys::Memory::protectMappedMemory(I->second, Flags);
    }

  private:
    std::map<void *, sys::MemoryBlock> Allocs;
  };

  static std::error_code doNothing() { return std::error_code(); }

  static TargetAddress reenter(void *JITTargetAddr, void *TrampolineAddr) {
    auto T = static_cast<OrcRemoteTargetServer *>(JITTargetAddr);
    auto AddrOrErr = T->requestCompile(
		       static_cast<TargetAddress>(
		         reinterpret_cast<uintptr_t>(TrampolineAddr)));
    // FIXME: Allow customizable failure substitution functions.
    assert(AddrOrErr && "Compile request failed");
    return *AddrOrErr;
  }

  ErrorOr<int32_t> handleCallIntVoid(TargetAddress Addr) {
    typedef int (*IntVoidFnTy)();
    IntVoidFnTy Fn =
        reinterpret_cast<IntVoidFnTy>(static_cast<uintptr_t>(Addr));

    DEBUG(dbgs() << "  Calling " << format("0x%016x", Addr) << "\n");
    int Result = Fn();
    DEBUG(dbgs() << "  Result = " << Result << "\n");

    return Result;
  }

  ErrorOr<int32_t> handleCallMain(TargetAddress Addr,
				  std::vector<std::string> Args) {
    typedef int (*MainFnTy)(int, const char *[]);

    MainFnTy Fn = reinterpret_cast<MainFnTy>(static_cast<uintptr_t>(Addr));
    int ArgC = Args.size() + 1;
    int Idx = 1;
    std::unique_ptr<const char *[]> ArgV(new const char *[ArgC + 1]);
    ArgV[0] = "<jit process>";
    for (auto &Arg : Args)
      ArgV[Idx++] = Arg.c_str();

    DEBUG(dbgs() << "  Calling " << format("0x%016x", Addr) << "\n");
    int Result = Fn(ArgC, ArgV.get());
    DEBUG(dbgs() << "  Result = " << Result << "\n");

    return Result;
  }

  std::error_code handleCallVoidVoid(TargetAddress Addr) {
    typedef void (*VoidVoidFnTy)();
    VoidVoidFnTy Fn =
        reinterpret_cast<VoidVoidFnTy>(static_cast<uintptr_t>(Addr));

    DEBUG(dbgs() << "  Calling " << format("0x%016x", Addr) << "\n");
    Fn();
    DEBUG(dbgs() << "  Complete.\n");

    return std::error_code();
  }

  std::error_code handleCreateRemoteAllocator(ResourceIdMgr::ResourceId Id) {
    auto I = Allocators.find(Id);
    if (I != Allocators.end())
      return orcError(OrcErrorCode::RemoteAllocatorIdAlreadyInUse);
    DEBUG(dbgs() << "  Created allocator " << Id << "\n");
    Allocators[Id] = Allocator();
    return std::error_code();
  }

  std::error_code handleCreateIndirectStubsOwner(ResourceIdMgr::ResourceId Id) {
    auto I = IndirectStubsOwners.find(Id);
    if (I != IndirectStubsOwners.end())
      return orcError(OrcErrorCode::RemoteIndirectStubsOwnerIdAlreadyInUse);
    DEBUG(dbgs() << "  Create indirect stubs owner " << Id << "\n");
    IndirectStubsOwners[Id] = ISBlockOwnerList();
    return std::error_code();
  }

  std::error_code handleDeregisterEHFrames(TargetAddress TAddr, uint32_t Size) {
    uint8_t *Addr = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(TAddr));
    DEBUG(dbgs() << "  Registering EH frames at " << format("0x%016x", TAddr)
                 << ", Size = " << Size << " bytes\n");
    EHFramesDeregister(Addr, Size);
    return std::error_code();
  }

  std::error_code handleDestroyRemoteAllocator(ResourceIdMgr::ResourceId Id) {
    auto I = Allocators.find(Id);
    if (I == Allocators.end())
      return orcError(OrcErrorCode::RemoteAllocatorDoesNotExist);
    Allocators.erase(I);
    DEBUG(dbgs() << "  Destroyed allocator " << Id << "\n");
    return std::error_code();
  }

  std::error_code
  handleDestroyIndirectStubsOwner(ResourceIdMgr::ResourceId Id) {
    auto I = IndirectStubsOwners.find(Id);
    if (I == IndirectStubsOwners.end())
      return orcError(OrcErrorCode::RemoteIndirectStubsOwnerDoesNotExist);
    IndirectStubsOwners.erase(I);
    return std::error_code();
  }

  ErrorOr<std::tuple<TargetAddress, TargetAddress, uint32_t>>
  handleEmitIndirectStubs(ResourceIdMgr::ResourceId Id,
			  uint32_t NumStubsRequired) {
    DEBUG(dbgs() << "  ISMgr " << Id << " request " << NumStubsRequired
                 << " stubs.\n");

    auto StubOwnerItr = IndirectStubsOwners.find(Id);
    if (StubOwnerItr == IndirectStubsOwners.end())
      return orcError(OrcErrorCode::RemoteIndirectStubsOwnerDoesNotExist);

    typename TargetT::IndirectStubsInfo IS;
    if (auto EC =
            TargetT::emitIndirectStubsBlock(IS, NumStubsRequired, nullptr))
      return EC;

    TargetAddress StubsBase =
        static_cast<TargetAddress>(reinterpret_cast<uintptr_t>(IS.getStub(0)));
    TargetAddress PtrsBase =
        static_cast<TargetAddress>(reinterpret_cast<uintptr_t>(IS.getPtr(0)));
    uint32_t NumStubsEmitted = IS.getNumStubs();

    auto &BlockList = StubOwnerItr->second;
    BlockList.push_back(std::move(IS));

    return std::make_tuple(StubsBase, PtrsBase, NumStubsEmitted);
  }

  std::error_code handleEmitResolverBlock() {
    std::error_code EC;
    ResolverBlock = sys::OwningMemoryBlock(sys::Memory::allocateMappedMemory(
        TargetT::ResolverCodeSize, nullptr,
        sys::Memory::MF_READ | sys::Memory::MF_WRITE, EC));
    if (EC)
      return EC;

    TargetT::writeResolverCode(static_cast<uint8_t *>(ResolverBlock.base()),
                               &reenter, this);

    return sys::Memory::protectMappedMemory(ResolverBlock.getMemoryBlock(),
                                            sys::Memory::MF_READ |
                                                sys::Memory::MF_EXEC);
  }

  ErrorOr<std::tuple<TargetAddress, uint32_t>>
  handleEmitTrampolineBlock() {
    std::error_code EC;
    auto TrampolineBlock =
        sys::OwningMemoryBlock(sys::Memory::allocateMappedMemory(
            sys::Process::getPageSize(), nullptr,
            sys::Memory::MF_READ | sys::Memory::MF_WRITE, EC));
    if (EC)
      return EC;

    uint32_t NumTrampolines =
        (sys::Process::getPageSize() - TargetT::PointerSize) /
        TargetT::TrampolineSize;

    uint8_t *TrampolineMem = static_cast<uint8_t *>(TrampolineBlock.base());
    TargetT::writeTrampolines(TrampolineMem, ResolverBlock.base(),
                              NumTrampolines);

    EC = sys::Memory::protectMappedMemory(TrampolineBlock.getMemoryBlock(),
                                          sys::Memory::MF_READ |
                                              sys::Memory::MF_EXEC);

    TrampolineBlocks.push_back(std::move(TrampolineBlock));

    auto TrampolineBaseAddr =
      static_cast<TargetAddress>(reinterpret_cast<uintptr_t>(TrampolineMem));

    return std::make_tuple(TrampolineBaseAddr, NumTrampolines);
  }

  ErrorOr<TargetAddress> handleGetSymbolAddress(const std::string &Name) {
    TargetAddress Addr = SymbolLookup(Name);
    DEBUG(dbgs() << "  Symbol '" << Name << "' =  " << format("0x%016x", Addr)
                 << "\n");
    return Addr;
  }

  ErrorOr<std::tuple<std::string, uint32_t, uint32_t, uint32_t, uint32_t>>
  handleGetRemoteInfo() {
    std::string ProcessTriple = sys::getProcessTriple();
    uint32_t PointerSize = TargetT::PointerSize;
    uint32_t PageSize = sys::Process::getPageSize();
    uint32_t TrampolineSize = TargetT::TrampolineSize;
    uint32_t IndirectStubSize = TargetT::IndirectStubsInfo::StubSize;
    DEBUG(dbgs() << "  Remote info:\n"
                 << "    triple             = '" << ProcessTriple << "'\n"
                 << "    pointer size       = " << PointerSize << "\n"
                 << "    page size          = " << PageSize << "\n"
                 << "    trampoline size    = " << TrampolineSize << "\n"
                 << "    indirect stub size = " << IndirectStubSize << "\n");
    return std::make_tuple(ProcessTriple, PointerSize, PageSize ,TrampolineSize,
			   IndirectStubSize);
  }

  ErrorOr<std::vector<char>>
  handleReadMem(TargetAddress RSrc, uint64_t Size) {
    char *Src = reinterpret_cast<char *>(static_cast<uintptr_t>(RSrc));

    DEBUG(dbgs() << "  Reading " << Size << " bytes from "
                 << format("0x%016x", RSrc) << "\n");

    std::vector<char> Buffer;
    Buffer.resize(Size);
    for (char *P = Src; Size != 0; --Size)
      Buffer.push_back(*P++);

    return Buffer;
  }

  std::error_code handleRegisterEHFrames(TargetAddress TAddr, uint32_t Size) {
    uint8_t *Addr = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(TAddr));
    DEBUG(dbgs() << "  Registering EH frames at " << format("0x%016x", TAddr)
                 << ", Size = " << Size << " bytes\n");
    EHFramesRegister(Addr, Size);
    return std::error_code();
  }

  ErrorOr<TargetAddress>
  handleReserveMem(ResourceIdMgr::ResourceId Id, uint64_t Size,
		   uint32_t Align) {
    auto I = Allocators.find(Id);
    if (I == Allocators.end())
      return orcError(OrcErrorCode::RemoteAllocatorDoesNotExist);
    auto &Allocator = I->second;
    void *LocalAllocAddr = nullptr;
    if (auto EC = Allocator.allocate(LocalAllocAddr, Size, Align))
      return EC;

    DEBUG(dbgs() << "  Allocator " << Id << " reserved " << LocalAllocAddr
                 << " (" << Size << " bytes, alignment " << Align << ")\n");

    TargetAddress AllocAddr =
        static_cast<TargetAddress>(reinterpret_cast<uintptr_t>(LocalAllocAddr));

    return AllocAddr;
  }

  std::error_code handleSetProtections(ResourceIdMgr::ResourceId Id,
                                       TargetAddress Addr, uint32_t Flags) {
    auto I = Allocators.find(Id);
    if (I == Allocators.end())
      return orcError(OrcErrorCode::RemoteAllocatorDoesNotExist);
    auto &Allocator = I->second;
    void *LocalAddr = reinterpret_cast<void *>(static_cast<uintptr_t>(Addr));
    DEBUG(dbgs() << "  Allocator " << Id << " set permissions on " << LocalAddr
                 << " to " << (Flags & sys::Memory::MF_READ ? 'R' : '-')
                 << (Flags & sys::Memory::MF_WRITE ? 'W' : '-')
                 << (Flags & sys::Memory::MF_EXEC ? 'X' : '-') << "\n");
    return Allocator.setProtections(LocalAddr, Flags);
  }

  std::error_code handleWriteMem(DirectBufferWriter DBW) {
    DEBUG(dbgs() << "  Writing " << DBW.getSize() << " bytes to "
	         << format("0x%016x", DBW.getDst()) << "\n");
    return std::error_code();
  }

  std::error_code handleWritePtr(TargetAddress Addr, TargetAddress PtrVal) {
    DEBUG(dbgs() << "  Writing pointer *" << format("0x%016x", Addr) << " = "
                 << format("0x%016x", PtrVal) << "\n");
    uintptr_t *Ptr =
        reinterpret_cast<uintptr_t *>(static_cast<uintptr_t>(Addr));
    *Ptr = static_cast<uintptr_t>(PtrVal);
    return std::error_code();
  }

  ChannelT &Channel;
  SymbolLookupFtor SymbolLookup;
  EHFrameRegistrationFtor EHFramesRegister, EHFramesDeregister;
  std::map<ResourceIdMgr::ResourceId, Allocator> Allocators;
  typedef std::vector<typename TargetT::IndirectStubsInfo> ISBlockOwnerList;
  std::map<ResourceIdMgr::ResourceId, ISBlockOwnerList> IndirectStubsOwners;
  sys::OwningMemoryBlock ResolverBlock;
  std::vector<sys::OwningMemoryBlock> TrampolineBlocks;
};

} // end namespace remote
} // end namespace orc
} // end namespace llvm

#undef DEBUG_TYPE

#endif
