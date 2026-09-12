#include "TxCoordinator.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace AetherSDR {

bool TxCoordinator::Operation::permitsDispatch(qint64 now) const
{
    if (!m_state || m_state->cancelled.load(std::memory_order_acquire)
        || now < m_state->startedMs) {
        return false;
    }
    // Subtract only after validating nonnegative, ordered clock values. Avoid
    // deadline addition, which overflows for a large trusted-clock test input.
    return m_state->maximumMs == 0
        || now - m_state->startedMs < m_state->maximumMs;
}

bool TxCoordinator::Operation::sameOperation(const Operation& other) const
{
    return m_state && m_state == other.m_state;
}

bool TxCoordinator::Operation::permitsCleanup() const
{
    if (!m_state) {
        return false;
    }
    const std::shared_ptr<Identity> identity = m_state->actor->coordinator.lock();
    return identity && identity->generation.load(std::memory_order_acquire) == m_state->generation;
}

bool TxCoordinator::Intent::pending() const
{
    return m_state && !m_state->ended.load(std::memory_order_acquire)
        && m_state->operation.permitsCleanup();
}

bool TxCoordinator::Intent::permitsDispatch(qint64 now) const
{
    return pending() && m_state->operation.permitsDispatch(now);
}

bool TxCoordinator::Intent::sameIntent(const Intent& other) const
{
    return m_state && m_state == other.m_state;
}

TxCoordinator::TxCoordinator(StopHandler stopHandler)
    : m_thread(QThread::currentThread())
    , m_identity(std::make_shared<Identity>())
    , m_stopHandler(std::move(stopHandler))
{
}

TxCoordinator::~TxCoordinator()
{
    // The owner performs reset while its backend is alive. Destruction is a
    // final fence only: callbacks into a partially destroyed owner are unsafe.
    if (m_active.m_state) {
        m_active.m_state->cancelled.store(true, std::memory_order_release);
    }
    if (m_stopping.m_state) {
        m_stopping.m_state->cancelled.store(true, std::memory_order_release);
    }
    for (const std::shared_ptr<IntentState>& intent : m_intents) {
        intent->ended.store(true, std::memory_order_release);
    }
}

bool TxCoordinator::onThread() const
{
    return QThread::currentThread() == m_thread;
}

bool TxCoordinator::validActor(const Actor& actor) const
{
    return actor.m_state && !actor.m_state->revoked
        && actor.m_state->coordinator.lock() == m_identity;
}

TxCoordinator::Actor TxCoordinator::registerActor(ActorPolicy policy)
{
    if (!onThread() || policy.maximumOperationMs < 0 || !m_stopHandler) {
        return {};
    }
    std::erase_if(m_actors, [](const std::weak_ptr<ActorState>& actor) {
        const std::shared_ptr<ActorState> state = actor.lock();
        return !state || state->revoked;
    });
    if (m_actors.size() >= kMaximumActors) {
        return {};
    }
    Actor actor;
    actor.m_state = std::make_shared<ActorState>(ActorState{m_identity, policy, false});
    m_actors.push_back(actor.m_state);
    return actor;
}

TxCoordinator::Admission TxCoordinator::acquire(const Actor& actor, qint64 now)
{
    if (!onThread()) {
        return {{}, Refusal::WrongThread};
    }
    if (!validActor(actor) || now < 0) {
        return {{}, Refusal::InvalidActor};
    }
    if (!actor.m_state->policy.mayTransmit) {
        return {{}, Refusal::Denied};
    }
    expire(now);
    // stopHandler may synchronously revoke the requesting actor.
    if (!validActor(actor)) {
        return {{}, Refusal::InvalidActor};
    }
    if (m_stopping.m_state || m_inStopHandler) {
        return {{}, Refusal::Recovering};
    }
    if (m_active.m_state) {
        if (m_active.m_state->actor != actor.m_state) {
            return {{}, Refusal::Busy};
        }
        return {m_active, Refusal::None};
    }
    if (m_unconfirmed.m_state && m_unconfirmed.m_state->actor != actor.m_state) {
        // Local queue completion says nothing about a radio-buffered tail.
        // Keep the preceding owner until qualified stop evidence arrives.
        return {{}, Refusal::Busy};
    }
    if (m_identity->generation.load() == std::numeric_limits<quint64>::max()) {
        return {{}, Refusal::Recovering};
    }
    m_active.m_state = std::make_shared<OperationState>();
    m_active.m_state->actor = actor.m_state;
    m_active.m_state->startedMs = m_unconfirmed.m_state
        ? m_unconfirmed.m_state->startedMs : now;
    m_active.m_state->maximumMs = actor.m_state->policy.maximumOperationMs;
    m_active.m_state->generation = ++m_identity->generation;
    m_unconfirmed = {};
    return {m_active, Refusal::None};
}

bool TxCoordinator::owns(const Actor& actor, const Operation& operation) const
{
    return onThread() && validActor(actor)
        && ((m_active.sameOperation(operation) && m_active.m_state->actor == actor.m_state)
            || (m_unconfirmed.sameOperation(operation)
                && m_unconfirmed.m_state->actor == actor.m_state));
}

TxCoordinator::Intent TxCoordinator::beginIntent(const Operation& operation,
                                                const Intent& previous, Activity activity)
{
    const unsigned activityBit = static_cast<unsigned>(activity);
    if (!onThread() || !m_active.sameOperation(operation)
        || m_active.m_state->cancelled.load(std::memory_order_acquire)
        || activityBit == 0 || activityBit > static_cast<unsigned>(Activity::Cwx)
        || (activityBit & (activityBit - 1)) != 0) {
        return {};
    }
    if (previous.pending()) {
        // An unrelated live handle is not this producer's reusable slot.
        if (previous.m_state->operation.sameOperation(operation)
            && previous.m_state->activity == activity
            && std::find(m_intents.begin(), m_intents.end(), previous.m_state) != m_intents.end()) {
            if (!previous.m_state->finishing) {
                return previous;
            }
        } else {
            return {};
        }
    }
    if (m_intents.size() >= kMaximumIntents) {
        return {};
    }
    Intent intent;
    intent.m_state = std::make_shared<IntentState>();
    intent.m_state->operation = operation;
    intent.m_state->activity = activity;
    m_intents.push_back(intent.m_state);
    return intent;
}

bool TxCoordinator::requestIntentEnd(const Intent& intent)
{
    if (!onThread() || !intent.m_state
        || std::find(m_intents.begin(), m_intents.end(), intent.m_state) == m_intents.end()) {
        return false;
    }
    intent.m_state->finishing = true;
    return true;
}

bool TxCoordinator::endIntent(const Intent& intent)
{
    if (!onThread() || !intent.m_state) {
        return false;
    }
    const auto found = std::find(m_intents.begin(), m_intents.end(), intent.m_state);
    if (found == m_intents.end()) {
        return false;
    }
    intent.m_state->ended.store(true, std::memory_order_release);
    m_intents.erase(found);
    return true;
}

bool TxCoordinator::hasIntents(const Operation& operation) const
{
    return onThread() && std::any_of(m_intents.begin(), m_intents.end(),
        [&operation](const std::shared_ptr<IntentState>& intent) {
            return intent->operation.sameOperation(operation);
        });
}

unsigned TxCoordinator::activeActivities(const Operation& operation) const
{
    if (!onThread()) {
        return 0;
    }
    unsigned activities = 0;
    for (const std::shared_ptr<IntentState>& intent : m_intents) {
        if (intent->operation.sameOperation(operation)) {
            activities |= static_cast<unsigned>(intent->activity);
        }
    }
    return activities;
}

void TxCoordinator::endIntents(const Operation& operation)
{
    std::erase_if(m_intents, [&operation](const std::shared_ptr<IntentState>& intent) {
        if (!intent->operation.sameOperation(operation)) {
            return false;
        }
        intent->ended.store(true, std::memory_order_release);
        return true;
    });
}

TxCoordinator::Operation TxCoordinator::cleanupFence() const
{
    if (!onThread()) {
        return {};
    }
    Operation fence;
    fence.m_state = std::make_shared<OperationState>();
    fence.m_state->actor = std::make_shared<ActorState>(ActorState{m_identity, {}, true});
    fence.m_state->cancelled.store(true, std::memory_order_release);
    fence.m_state->generation = m_identity->generation.load(std::memory_order_acquire);
    return fence;
}

bool TxCoordinator::finishLocalIntent(const Operation& operation)
{
    if (!onThread() || !m_active.sameOperation(operation) || hasIntents(operation)) {
        return false;
    }
    m_active.m_state->cancelled.store(true, std::memory_order_release);
    m_unconfirmed = m_active;
    m_active = {};
    return true;
}

void TxCoordinator::stop(StopReason reason)
{
    if (!m_active.m_state && !m_unconfirmed.m_state) {
        return;
    }
    m_stopping = m_active.m_state ? m_active : m_unconfirmed;
    m_active = {};
    m_unconfirmed = {};
    m_stopping.m_state->cancelled.store(true, std::memory_order_release);
    // Publish recovery state before invoking user code. Reentrant requests may
    // not acquire and get unkeyed by cleanup for the preceding operation.
    const Operation stopping = m_stopping;
    m_inStopHandler = true;
    m_stopHandler(stopping, reason);
    endIntents(stopping);
    m_inStopHandler = false;
}

bool TxCoordinator::cancel(const Actor& actor, const Operation& operation)
{
    if (!owns(actor, operation)) {
        return false;
    }
    stop(StopReason::OwnerCancelled);
    return true;
}

void TxCoordinator::revoke(const Actor& actor)
{
    if (!onThread() || !validActor(actor)) {
        return;
    }
    actor.m_state->revoked = true;
    if ((m_active.m_state && m_active.m_state->actor == actor.m_state)
        || (m_unconfirmed.m_state && m_unconfirmed.m_state->actor == actor.m_state)) {
        stop(StopReason::ActorRevoked);
    }
}

void TxCoordinator::expire(qint64 now)
{
    if (!onThread()) {
        return;
    }
    if (m_active.m_state && !m_active.permitsDispatch(now)) {
        stop(StopReason::Expired);
    } else if (m_unconfirmed.m_state) {
        // Its dispatch fence is already cancelled, so check the original
        // deadline rather than treating cancellation itself as expiration.
        const OperationState& state = *m_unconfirmed.m_state;
        if (now < state.startedMs
            || (state.maximumMs > 0 && now - state.startedMs >= state.maximumMs)) {
            stop(StopReason::Expired);
        }
    }
}

void TxCoordinator::reset()
{
    if (onThread()) {
        stop(StopReason::Reset);
        // A transport can survive a reconnect. Fence old queued key-ups too,
        // before that same socket is ever reused for a new radio connection.
        if (m_identity->generation.load() < std::numeric_limits<quint64>::max()) {
            ++m_identity->generation;
        }
    }
}

void TxCoordinator::emergencyStop()
{
    if (onThread()) {
        stop(StopReason::Emergency);
    }
}

bool TxCoordinator::acknowledgeStopped(const Operation& operation)
{
    if (!onThread()) {
        return false;
    }
    if (m_stopping.sameOperation(operation)) {
        m_stopping = {};
        return true;
    }
    if (m_unconfirmed.sameOperation(operation)) {
        m_unconfirmed = {};
        return true;
    }
    return false;
}

bool TxCoordinator::recovering() const
{
    return onThread() && bool(m_stopping.m_state);
}

} // namespace AetherSDR
