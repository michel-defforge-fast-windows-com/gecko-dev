/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim: set ts=8 sts=4 et sw=4 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ForkServiceChild.h"
#include "ForkServer.h"
#include "mozilla/ipc/IPDLParamTraits.h"
#include "mozilla/Logging.h"
#include "mozilla/ipc/GeckoChildProcessHost.h"

#include <unistd.h>
#include <fcntl.h>

namespace mozilla {
namespace ipc {

extern LazyLogModule gForkServiceLog;

mozilla::UniquePtr<ForkServiceChild> ForkServiceChild::sForkServiceChild;

void
ForkServiceChild::StartForkServer() {
    std::vector<std::string> extraArgs;

    GeckoChildProcessHost *subprocess =
        new GeckoChildProcessHost(GeckoProcessType_ForkServer, false);
    subprocess->LaunchAndWaitForProcessHandle(std::move(extraArgs));

    int fd = subprocess->GetChannel()->GetFileDescriptor();
    fd = dup(fd);               // Dup it because the channel will close it.
    int fs_flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fs_flags & ~O_NONBLOCK);
    int fd_flags = fcntl(fd, F_GETFD, 0);
    fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);

    sForkServiceChild = mozilla::MakeUnique<ForkServiceChild>(fd, subprocess);

    // Without doing this, IO thread may intercept messages since the
    // IPC::Channel created by it is still open.
    subprocess->GetChannel()->Close();
}

void
ForkServiceChild::StopForkServer() {
    sForkServiceChild = nullptr;
}

ForkServiceChild::ForkServiceChild(int aFd, GeckoChildProcessHost* aProcess)
    : mWaitForHello(true)
    , mProcess(aProcess) {
    mTcver = MakeUnique<MiniTransceiver>(aFd);
}

ForkServiceChild::~ForkServiceChild() {
    mProcess->Destroy();
    close(mTcver->GetFD());
}

bool
ForkServiceChild::SendForkNewSubprocess(const nsTArray<nsCString>& aArgv,
                                        const nsTArray<EnvVar>& aEnvMap,
                                        const nsTArray<FdMapping>& aFdsRemap,
                                        pid_t* aPid) {
    if (mWaitForHello) {
        // IPC::Channel created by the GeckoChildProcessHost has
        // already send a HELLO.  It is expected to receive a hello
        // message from the fork server too.
        IPC::Message hello;
        mTcver->RecvInfallible(hello, "Fail to receive HELLO message");
        MOZ_ASSERT(hello.type() == ForkServer::kHELLO_MESSAGE_TYPE);
        mWaitForHello = false;
    }

    mRecvPid = -1;
    IPC::Message msg(MSG_ROUTING_CONTROL, Msg_ForkNewSubprocess__ID);

    WriteIPDLParam(&msg, nullptr, aArgv);
    WriteIPDLParam(&msg, nullptr, aEnvMap);
    WriteIPDLParam(&msg, nullptr, aFdsRemap);
    if (!mTcver->Send(msg)) {
        MOZ_LOG(gForkServiceLog, LogLevel::Verbose,
                ("the pipe to the fork server is closed or having errors"));
        return false;
    }

    IPC::Message reply;
    if (!mTcver->Recv(reply)) {
        MOZ_LOG(gForkServiceLog, LogLevel::Verbose,
                ("the pipe to the fork server is closed or having errors"));
        return false;
    }
    OnMessageReceived(std::move(reply));

    MOZ_ASSERT(mRecvPid != -1);
    *aPid = mRecvPid;
    return true;
}

void
ForkServiceChild::OnMessageReceived(IPC::Message&& message) {
    if (message.type() != Reply_ForkNewSubprocess__ID) {
        MOZ_LOG(gForkServiceLog, LogLevel::Verbose,
                ("unknown reply type %d", message.type()));
        return;
    }
    PickleIterator iter__(message);

    if (!ReadIPDLParam(&message, &iter__, nullptr, &mRecvPid)) {
        MOZ_CRASH("Error deserializing 'pid_t'");
    }
    message.EndRead(iter__, message.type());
}

NS_IMPL_ISUPPORTS(ForkServerLauncher, nsIObserver)

bool ForkServerLauncher::mHaveStartedClient = false;
StaticRefPtr<ForkServerLauncher> ForkServerLauncher::mSingleton;

ForkServerLauncher::ForkServerLauncher() {
}

ForkServerLauncher::~ForkServerLauncher() {
}

already_AddRefed<ForkServerLauncher>
ForkServerLauncher::Create() {
    if (mSingleton == nullptr) {
        mSingleton = new ForkServerLauncher();
    }
    RefPtr<ForkServerLauncher> launcher = mSingleton;
    return launcher.forget();
}

NS_IMETHODIMP
ForkServerLauncher::Observe(nsISupports* aSubject,
                           const char* aTopic,
                           const char16_t* aData) {
    if (!mHaveStartedClient && strcmp(aTopic, NS_XPCOM_STARTUP_CATEGORY) == 0) {
        mHaveStartedClient = true;
        ForkServiceChild::StartForkServer();

        nsCOMPtr<nsIObserverService> obsSvc = mozilla::services::GetObserverService();
        MOZ_ASSERT(obsSvc != nullptr);
        obsSvc->AddObserver(this, NS_XPCOM_SHUTDOWN_OBSERVER_ID, false);
    }

    if (mHaveStartedClient && strcmp(aTopic, NS_XPCOM_SHUTDOWN_OBSERVER_ID) == 0) {
        mHaveStartedClient = false;
        ForkServiceChild::StopForkServer();
    }
    return NS_OK;
}

}
}
