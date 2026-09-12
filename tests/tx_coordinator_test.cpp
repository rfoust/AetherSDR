#include "core/TxCoordinator.h"

#include <QCoreApplication>
#include <QThread>

#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

using AetherSDR::TxCoordinator;

namespace {
int failures = 0;
void check(bool condition, const char* message)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    failures += !condition;
}

void ownershipAndRecovery()
{
    int stops = 0;
    TxCoordinator::Operation stopped;
    TxCoordinator::Actor competitor;
    TxCoordinator* callbackCoordinator = nullptr;
    TxCoordinator coordinator([&](const TxCoordinator::Operation& operation,
                                 TxCoordinator::StopReason) {
        ++stops;
        stopped = operation;
        check(!operation.permitsDispatch(100), "stop invalidates delivery before callback");
        check(callbackCoordinator->acquire(competitor, 100).refusal == TxCoordinator::Refusal::Recovering,
              "reentrant competing start cannot race stop cleanup");
    });
    callbackCoordinator = &coordinator;
    const TxCoordinator::Actor owner = coordinator.registerActor({true, 0});
    competitor = coordinator.registerActor({true, 0});
    const TxCoordinator::Actor observer = coordinator.registerActor({false, 0});
    check(coordinator.acquire({}, 0).refusal == TxCoordinator::Refusal::InvalidActor,
          "default actor has no authority");
    check(coordinator.acquire(observer, 0).refusal == TxCoordinator::Refusal::Denied,
          "authenticated actor without transmit permission cannot acquire");
    const TxCoordinator::Admission first = coordinator.acquire(owner, 0);
    check(first.accepted() && first.operation.permitsDispatch(0), "owner acquires usable operation");
    check(stops == 0, "acquiring never dispatches stop or keying");
    check(coordinator.acquire(competitor, 1).refusal == TxCoordinator::Refusal::Busy,
          "second actor cannot take the owner slot");
    check(coordinator.acquire(owner, 2).operation.sameOperation(first.operation),
          "repeated start retains the existing operation");
    check(!coordinator.cancel(competitor, first.operation), "nonowner cannot stop owner");
    check(!coordinator.finishLocalIntent({}), "invalid handle cannot finish another operation");
    check(coordinator.cancel(owner, first.operation) && stops == 1,
          "owner cancellation dispatches one stop");
    check(!coordinator.cancel(owner, first.operation) && stops == 1,
          "duplicate cancellation is inert");
    check(!coordinator.acknowledgeStopped({}), "unrelated stop acknowledgment cannot clear recovery");
    check(coordinator.acknowledgeStopped(stopped), "matching qualified stop acknowledgment clears recovery");
    const TxCoordinator::Admission next = coordinator.acquire(competitor, 101);
    check(next.accepted(), "next actor can acquire after recovery");
    check(!coordinator.finishLocalIntent(first.operation), "late old completion cannot stop new owner");
    check(!coordinator.acknowledgeStopped(stopped), "late old acknowledgment is inert");
    check(next.operation.permitsDispatch(102), "new owner survives old callbacks");
    check(coordinator.finishLocalIntent(next.operation), "local intent completes normally");
    check(!next.operation.permitsDispatch(103), "normal completion fences queued work");
    check(next.operation.permitsCleanup(), "normal completion retains queued key-up cleanup");
    check(coordinator.acquire(owner, 104).refusal == TxCoordinator::Refusal::Busy,
          "local completion cannot authorize a different owner");
    check(coordinator.acknowledgeStopped(next.operation), "qualified normal stop releases the owner slot");
    check(coordinator.acquire(owner, 104).accepted(), "new owner starts after qualified completion");
    check(!next.operation.permitsCleanup(), "old queued key-up cannot unkey a newer operation");
    check(stops == 1, "normal completion never sends an extra unkey");
}

void expiryAndRevocation()
{
    int stops = 0;
    TxCoordinator::Operation stopped;
    TxCoordinator::StopReason reason = TxCoordinator::StopReason::Reset;
    TxCoordinator coordinator([&](const TxCoordinator::Operation& operation,
                                 TxCoordinator::StopReason why) {
        ++stops;
        stopped = operation;
        reason = why;
    });
    const TxCoordinator::Actor bounded = coordinator.registerActor({true, 20});
    const TxCoordinator::Actor other = coordinator.registerActor({true, 0});
    const TxCoordinator::Admission first = coordinator.acquire(bounded, 100);
    check(first.operation.permitsDispatch(119), "bounded operation survives before deadline");
    check(!first.operation.permitsDispatch(120), "transport fence rejects exact deadline before timer runs");
    check(coordinator.acquire(bounded, 119).accepted(), "repeat admission before deadline succeeds");
    coordinator.expire(120);
    check(stops == 1 && reason == TxCoordinator::StopReason::Expired,
          "repeat acquisition cannot extend maximum continuous duration");
    coordinator.expire(121);
    check(stops == 1, "expired operation stops only once");
    check(coordinator.acknowledgeStopped(stopped), "expiry recovery acknowledged");
    const TxCoordinator::Admission second = coordinator.acquire(bounded, 122);
    coordinator.revoke(other);
    check(second.operation.permitsDispatch(123) && stops == 1,
          "revoking another actor does not affect owner");
    coordinator.revoke(bounded);
    check(stops == 2 && reason == TxCoordinator::StopReason::ActorRevoked,
          "owner revocation cancels active transmission");
    check(!second.operation.permitsDispatch(123), "revocation fences pending output");
    check(coordinator.acknowledgeStopped(stopped), "revocation recovery acknowledged");
    check(coordinator.acquire(bounded, 124).refusal == TxCoordinator::Refusal::InvalidActor,
          "revoked actor cannot reacquire");
}

void unconfirmedCompletion()
{
    int stops = 0;
    TxCoordinator coordinator([&](const auto&, auto) { ++stops; });
    const TxCoordinator::Actor owner = coordinator.registerActor({true, 0});
    const TxCoordinator::Actor other = coordinator.registerActor({true, 0});
    const TxCoordinator::Operation first = coordinator.acquire(owner, 0).operation;
    check(!coordinator.acknowledgeStopped(first), "active intent cannot be acknowledged as stopped");
    check(coordinator.finishLocalIntent(first), "local completion records the preceding owner");
    check(!coordinator.finishLocalIntent(first), "duplicate local completion is inert");
    check(coordinator.owns(owner, first) && !coordinator.owns(other, first),
          "unconfirmed tail retains only its original owner's cancellation authority");
    check(!first.permitsDispatch(1) && first.permitsCleanup(),
          "unconfirmed tail admits cleanup but cannot send more key-on work");
    check(!coordinator.recovering() && stops == 0,
          "normal completion does not force cleanup or latch operator admission");
    check(coordinator.acquire(other, 1).refusal == TxCoordinator::Refusal::Busy,
          "a competitor cannot treat local drain as radio-idle evidence");
    check(!coordinator.cancel(other, first), "competitor cannot cancel an unconfirmed tail");
    std::unique_ptr<QThread> worker(QThread::create([&] {
        check(!coordinator.finishLocalIntent(first) && !coordinator.acknowledgeStopped(first)
                  && !coordinator.cancel(owner, first),
              "worker-thread completion, acknowledgment and cancellation cannot mutate a tail");
    }));
    worker->start();
    worker->wait();
    check(coordinator.owns(owner, first), "off-thread calls leave unconfirmed ownership intact");
    const TxCoordinator::Operation second = coordinator.acquire(owner, 2).operation;
    check(second.permitsDispatch(std::numeric_limits<qint64>::max())
              && !second.sameOperation(first) && !first.permitsCleanup(),
          "same unbounded operator reengages with a new queue generation and no timeout");
    check(!coordinator.acknowledgeStopped(first) && !coordinator.finishLocalIntent(first),
          "an old completion cannot release ownership after same-actor reengagement");
    check(coordinator.finishLocalIntent(second), "reengaged operator can release again");
    check(!coordinator.acknowledgeStopped(first)
              && coordinator.acquire(other, 3).refusal == TxCoordinator::Refusal::Busy,
          "an old readback cannot release a newer unconfirmed tail");
    check(coordinator.acknowledgeStopped(second), "only matching qualified completion releases ownership");
    check(coordinator.acquire(other, 4).accepted(), "handoff is available after the qualified stop");
    check(stops == 0, "qualified normal completion never sends a redundant stop");
}

void unconfirmedStopSources()
{
    for (const TxCoordinator::StopReason reason : {TxCoordinator::StopReason::OwnerCancelled,
             TxCoordinator::StopReason::ActorRevoked, TxCoordinator::StopReason::Reset,
             TxCoordinator::StopReason::Emergency}) {
        int stops = 0;
        TxCoordinator::Operation stopped;
        TxCoordinator::StopReason actual = TxCoordinator::StopReason::Expired;
        TxCoordinator* current = nullptr;
        TxCoordinator::Actor other;
        TxCoordinator coordinator([&](const TxCoordinator::Operation& operation, TxCoordinator::StopReason why) {
            ++stops;
            stopped = operation;
            actual = why;
            check(current->recovering() && !operation.permitsDispatch(2),
                  "a tail stop publishes recovery and cancellation before callback");
            check(current->acquire(other, 2).refusal == TxCoordinator::Refusal::Recovering,
                  "tail cleanup cannot race reentrant admission");
        });
        current = &coordinator;
        const TxCoordinator::Actor owner = coordinator.registerActor({true, 0});
        other = coordinator.registerActor({true, 0});
        const TxCoordinator::Operation operation = coordinator.acquire(owner, 0).operation;
        check(coordinator.finishLocalIntent(operation), "stop source fixture has an unconfirmed tail");
        switch (reason) {
        case TxCoordinator::StopReason::OwnerCancelled:
            check(coordinator.cancel(owner, operation), "owner may cancel its unconfirmed tail");
            break;
        case TxCoordinator::StopReason::ActorRevoked:
            coordinator.revoke(owner);
            break;
        case TxCoordinator::StopReason::Reset:
            coordinator.reset();
            break;
        case TxCoordinator::StopReason::Emergency:
            coordinator.emergencyStop();
            break;
        case TxCoordinator::StopReason::Expired:
            break;
        }
        check(stops == 1 && actual == reason && stopped.sameOperation(operation),
              "every stop source retains the exact unconfirmed operation");
        coordinator.emergencyStop();
        check(stops == 1, "duplicate tail stop is inert");
        check(coordinator.acknowledgeStopped(stopped), "tail recovery accepts matching qualified acknowledgment");
        check(coordinator.acquire(other, 3).accepted(), "tail recovery does not permanently latch admission");
    }
}

void unconfirmedDeadline()
{
    int stops = 0;
    TxCoordinator::Operation stopped;
    TxCoordinator coordinator([&](const TxCoordinator::Operation& operation, TxCoordinator::StopReason reason) {
        ++stops;
        stopped = operation;
        check(reason == TxCoordinator::StopReason::Expired, "unconfirmed deadline uses expiry cleanup");
    });
    const TxCoordinator::Actor owner = coordinator.registerActor({true, 20});
    const TxCoordinator::Operation first = coordinator.acquire(owner, 100).operation;
    check(coordinator.finishLocalIntent(first), "bounded operation finishes local intent");
    coordinator.expire(118);
    check(stops == 0, "local fence cancellation is not itself deadline expiry");
    const TxCoordinator::Operation second = coordinator.acquire(owner, 119).operation;
    check(second.permitsDispatch(119) && !second.permitsDispatch(120),
          "reengagement without stop evidence cannot renew the original deadline");
    check(coordinator.finishLocalIntent(second), "bounded reengagement finishes local intent");
    coordinator.expire(120);
    check(stops == 1 && stopped.sameOperation(second), "an unconfirmed tail expires at the original deadline");
    check(coordinator.acknowledgeStopped(stopped), "expired tail acknowledges through the existing recovery path");
    const TxCoordinator::Operation third = coordinator.acquire(owner, 121).operation;
    check(third.permitsDispatch(140) && !third.permitsDispatch(141),
          "qualified recovery permits a fresh bounded duration");
    check(coordinator.finishLocalIntent(third), "fresh duration ends local intent before acquire-time expiry");
    check(coordinator.acquire(owner, 141).refusal == TxCoordinator::Refusal::Recovering && stops == 2,
          "acquisition cannot bypass an expired unconfirmed tail before the timer runs");
}

void lifetimeAndIdentity()
{
    TxCoordinator::Operation stale;
    TxCoordinator::Actor staleActor;
    {
        TxCoordinator coordinator([](const auto&, auto) {});
        staleActor = coordinator.registerActor({true, 0});
        stale = coordinator.acquire(staleActor, 0).operation;
    }
    check(!stale.permitsDispatch(1), "coordinator destruction fences retained worker handles");
    TxCoordinator other([](const auto&, auto) {});
    check(other.acquire(staleActor, 1).refusal == TxCoordinator::Refusal::InvalidActor,
          "actor from destroyed or different coordinator cannot be adopted");
    const TxCoordinator::Actor actor = other.registerActor({true, 0});
    const TxCoordinator::Admission operation = other.acquire(actor, 0);
    check(operation.operation.permitsDispatch(std::numeric_limits<qint64>::max()),
          "ordinary local operation has no newly imposed timeout");
    other.reset();
    check(!operation.operation.permitsDispatch(1), "connection reset invalidates old work");
    check(!operation.operation.permitsCleanup(), "connection reset invalidates old key-ups too");
    check(other.recovering(), "reset remains fail closed until cleanup is acknowledged");
    check(other.acknowledgeStopped(operation.operation), "reset cleanup tied to exact old operation");
    const TxCoordinator::Admission next = other.acquire(actor, 2);
    check(!next.operation.sameOperation(operation.operation), "new connection uses new operation identity");
    other.emergencyStop();
    check(!next.operation.permitsDispatch(3), "emergency stop invalidates active operation");

    TxCoordinator::Operation tail;
    {
        TxCoordinator completed([](const auto&, auto) {});
        tail = completed.acquire(completed.registerActor({true, 0}), 0).operation;
        check(completed.finishLocalIntent(tail), "lifetime fixture ends local intent");
    }
    check(!tail.permitsDispatch(1) && !tail.permitsCleanup(),
          "destruction retires unconfirmed tail handles too");
}

void limitsAndThread()
{
    TxCoordinator coordinator([](const auto&, auto) {});
    check(coordinator.acquire(coordinator.registerActor({true, -1}), 0).refusal
              == TxCoordinator::Refusal::InvalidActor, "negative time policy is rejected");
    std::vector<TxCoordinator::Actor> actors;
    for (int i = 0; i < TxCoordinator::kMaximumActors; ++i) {
        actors.push_back(coordinator.registerActor({true, 0}));
    }
    check(coordinator.acquire(coordinator.registerActor({true, 0}), 0).refusal
              == TxCoordinator::Refusal::InvalidActor, "actor registry is bounded");
    coordinator.revoke(actors.front());
    const TxCoordinator::Actor replacement = coordinator.registerActor({true, 0});
    check(coordinator.acquire(replacement, 0).accepted(), "revocation frees a registration slot");
    std::unique_ptr<QThread> thread(QThread::create([&] {
        check(coordinator.acquire(replacement, 1).refusal == TxCoordinator::Refusal::WrongThread,
              "off-thread admission fails in release builds");
        coordinator.emergencyStop();
    }));
    thread->start();
    thread->wait();
    check(coordinator.acquire(replacement, 2).accepted(), "off-thread mutation did not change state");

    TxCoordinator large([](const auto&, auto) {});
    const auto bounded = large.registerActor({true, 20});
    const auto result = large.acquire(bounded, std::numeric_limits<qint64>::max() - 10);
    check(result.operation.permitsDispatch(std::numeric_limits<qint64>::max()),
          "large clock values do not overflow deadline addition");
    check(!result.operation.permitsDispatch(-1), "backwards or invalid clock fails closed");
}

void acknowledgedCallbackCannotReenter()
{
    TxCoordinator::Actor actor;
    TxCoordinator* callbackCoordinator = nullptr;
    TxCoordinator coordinator([&](const TxCoordinator::Operation& operation,
                                 TxCoordinator::StopReason) {
        check(callbackCoordinator->acknowledgeStopped(operation), "synchronous stop may acknowledge cleanup");
        check(callbackCoordinator->acquire(actor, 2).refusal == TxCoordinator::Refusal::Recovering,
              "acknowledgment cannot admit new work inside old stop handler");
    });
    callbackCoordinator = &coordinator;
    actor = coordinator.registerActor({true, 0});
    const TxCoordinator::Operation before = coordinator.acquire(actor, 0).operation;
    coordinator.reset();
    const TxCoordinator::Admission after = coordinator.acquire(actor, 3);
    check(after.accepted() && after.operation.permitsCleanup(),
          "reset cannot invalidate a reentrant new operation");
    check(!before.permitsCleanup(), "reset fences the old generation after synchronous cleanup");
}

void stopOnlyFences()
{
    TxCoordinator coordinator([](const auto&, auto) {});
    const TxCoordinator::Operation fence = coordinator.cleanupFence();
    check(fence.permitsCleanup() && !fence.permitsDispatch(0), "idle cleanup fence cannot authorize key-on");
    check(!coordinator.finishLocalIntent(fence) && !coordinator.acknowledgeStopped(fence),
          "cleanup fence grants no ownership or recovery authority");
    const TxCoordinator::Actor actor = coordinator.registerActor({true, 0});
    check(coordinator.acquire(actor, 0).accepted(), "owner starts after idle cleanup fence");
    check(!fence.permitsCleanup(), "idle queued key-up cannot affect a newer operation");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ownershipAndRecovery();
    expiryAndRevocation();
    unconfirmedCompletion();
    unconfirmedStopSources();
    unconfirmedDeadline();
    lifetimeAndIdentity();
    limitsAndThread();
    acknowledgedCallbackCannotReenter();
    stopOnlyFences();
    return failures ? 1 : 0;
}
