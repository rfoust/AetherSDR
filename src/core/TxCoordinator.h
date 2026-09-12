#pragma once

#include <QThread>
#include <QMetaType>
#include <QtGlobal>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace AetherSDR {

// Engine-local authority, not a protocol credential or a PttSource. Only
// trusted composition code registers actors. Neither handle is constructible
// from client-supplied IDs; handles from another coordinator are rejected.
class TxCoordinator final {
    struct Identity;
    struct ActorState;
    struct OperationState;
    struct IntentState;
    struct ProducerState;

public:
    enum class Activity : unsigned { Mox = 1, Tune = 2, Atu = 4, CwKey = 8, CwPtt = 16, Cwx = 32 };
    class Operation;
    class Context;
    // A producer is a trusted in-process lifetime, not a client-supplied ID
    // or an independent TX actor. Copies cannot renew an invalidated lifetime.
    class Producer {
    public:
        [[nodiscard]] bool valid() const;
        [[nodiscard]] bool sameProducer(const Producer& other) const;
        void invalidate() const; // atomic; callable by the producer's destructor
    private:
        friend class TxCoordinator;
        friend class Context;
        std::shared_ptr<ProducerState> m_state;
    };
    class Actor {
    public:
        Actor() = default;
    private:
        friend class TxCoordinator;
        std::shared_ptr<ActorState> m_state;
    };

    // Hold across the actual terminal write, not across a queued callback.
    // Cancellation cannot retract a write already entered; this guard keeps
    // that fact visible until it returns. No worker waits for the owner thread.
    class Dispatch {
    public:
        Dispatch() = default;
        ~Dispatch();
        Dispatch(Dispatch&& other) noexcept;
        Dispatch& operator=(Dispatch&& other) noexcept;
        Dispatch(const Dispatch&) = delete;
        Dispatch& operator=(const Dispatch&) = delete;
        explicit operator bool() const { return bool(m_identity); }
    private:
        friend class TxCoordinator;
        friend class Operation;
        friend class Context;
        explicit Dispatch(std::shared_ptr<Identity> identity, bool continuous = false);
        std::shared_ptr<Identity> m_identity;
        bool m_continuous{false};
    };

    class Operation {
    public:
        Operation() = default;
        // Transport workers may only read this cancellation fence. Admission,
        // completion and all model access remain on the engine owning thread.
        [[nodiscard]] bool permitsDispatch(qint64 monotonicMs) const;
        // A queued key-up may outlive normal completion, but never a new
        // operation, connection reset, or coordinator destruction.
        [[nodiscard]] bool permitsCleanup() const;
        [[nodiscard]] bool sameOperation(const Operation& other) const;
        [[nodiscard]] Dispatch beginDispatch(qint64 monotonicMs, bool keying = true) const;
    private:
        friend class TxCoordinator;
        std::shared_ptr<OperationState> m_state;
    };

    // Local queue bookkeeping, never radio-idle evidence. Every copy shares a
    // once-only completion; dropping a queued item completes it too. The
    // trusted caller supplies a callback that marshals to its owner thread.
    class Completion {
        struct State {
            explicit State(std::function<void()> callback) : done(std::move(callback)) {}
            ~State() { finish(); }
            void finish()
            {
                if (!finished.exchange(true) && done) {
                    done();
                }
            }
            std::atomic<bool> finished{false};
            const std::function<void()> done;
        };
    public:
        Completion() = default;
        explicit Completion(std::function<void()> done)
            : m_state(std::make_shared<State>(std::move(done))) {}
        void finish() const { if (m_state) { m_state->finish(); } }
    private:
        std::shared_ptr<State> m_state;
    };

    // A typed command carries its original operation across scheduling and
    // replay. Cleanup may outlive normal completion but never a new operation.
    struct Command {
        Operation operation;
        bool keying{true};
        Completion completion{};
        [[nodiscard]] bool permitsDispatch(qint64 now) const
        {
            return keying ? operation.permitsDispatch(now) : operation.permitsCleanup();
        }
        [[nodiscard]] Dispatch beginDispatch(qint64 now) const
        {
            return operation.beginDispatch(now, keying);
        }
    };

    // One producer's contribution to an admitted operation. The producer keeps
    // its own handle and passes copies to delayed release callbacks. A handle
    // is not an actor grant, and ending it is not radio-idle evidence.
    class Intent {
    public:
        Intent() = default;
        [[nodiscard]] bool pending() const;
        [[nodiscard]] bool permitsDispatch(qint64 monotonicMs) const;
        [[nodiscard]] bool sameIntent(const Intent& other) const;
    private:
        friend class TxCoordinator;
        std::shared_ptr<IntentState> m_state;
    };

    // Immutable provenance for a queued audio block. Continuous microphone
    // media is explicitly granted for VOX/RX metering; other media must carry
    // its original admitted operation. Neither kind can acquire or key TX.
    class Context {
    public:
        [[nodiscard]] bool permitsDispatch(qint64 monotonicMs) const;
        [[nodiscard]] Dispatch beginDispatch(qint64 monotonicMs) const;
        [[nodiscard]] bool sameContext(const Context& other) const;
    private:
        friend class TxCoordinator;
        Producer m_producer;
        Operation m_operation;
        quint64 m_session{0};
        bool m_continuous{false};
    };

    struct ActorPolicy {
        bool mayTransmit{false};
        // Zero means no new timeout for an existing local operator workflow.
        // A bounded actor cannot extend a transmission by repeating acquire().
        qint64 maximumOperationMs{0};
    };

    enum class Refusal { None, WrongThread, InvalidActor, Denied, Busy, Recovering };
    struct Admission {
        Operation operation;
        Refusal refusal{Refusal::InvalidActor};
        [[nodiscard]] bool accepted() const { return refusal == Refusal::None; }
    };

    enum class StopReason { OwnerCancelled, ActorRevoked, Expired, Reset, Emergency };
    using StopHandler = std::function<void(const Operation&, StopReason)>;
    static constexpr int kMaximumActors = 64;
    static constexpr int kMaximumIntents = 256;
    static constexpr int kMaximumProducers = 256;

    explicit TxCoordinator(StopHandler stopHandler);
    [[nodiscard]] static qint64 monotonicMs();
    ~TxCoordinator();
    TxCoordinator(const TxCoordinator&) = delete;
    TxCoordinator& operator=(const TxCoordinator&) = delete;

    [[nodiscard]] Actor registerActor(ActorPolicy policy);
    [[nodiscard]] Producer registerProducer(bool continuousMicrophone = false);
    [[nodiscard]] Context mediaContext(const Producer& producer, const Operation& operation = {}) const;
    [[nodiscard]] Admission acquire(const Actor& actor, qint64 monotonicMs);
    // Repeated admission by the same producer reuses its live handle; it does
    // not accumulate reference-counted holds. Distinct producers use distinct
    // handles even when sharing the transitional desktop actor/operation.
    [[nodiscard]] Intent beginIntent(const Operation& operation, const Intent& previous, Activity activity);
    // Mark release BEFORE invoking callbacks or enqueueing cleanup. A new
    // request then gets a distinct handle while this one's queued tail drains.
    [[nodiscard]] bool requestIntentEnd(const Intent& intent);
    [[nodiscard]] bool endIntent(const Intent& intent);
    [[nodiscard]] bool hasIntents(const Operation& operation) const;
    [[nodiscard]] unsigned activeActivities(const Operation& operation) const;
    // Stop-only delivery fence, including when no operation was acquired.
    // It conveys no key-on, ownership, completion or acknowledgment authority.
    [[nodiscard]] Operation cleanupFence() const;
    // Ends local intent and fences queued key-on, but does NOT assert radio
    // idle. Until qualified acknowledgment, only this same actor can start
    // another operation (the transitional desktop compatibility workflow).
    // A bounded actor retains its original deadline across unconfirmed tails.
    [[nodiscard]] bool finishLocalIntent(const Operation& operation);
    // Cancellation invalidates queued work before the engine's immediate stop.
    [[nodiscard]] bool cancel(const Actor& actor, const Operation& operation);
    void revoke(const Actor& actor);
    void expire(qint64 monotonicMs);
    void reset();
    void emergencyStop();
    // Qualified readback or transport teardown must precede acknowledgment.
    // Merely requesting unkey or draining a local queue is not proof. A matching
    // acknowledgment clears either forced-stop recovery or an unconfirmed local
    // completion. An old completion cannot acknowledge a newer operation.
    //
    // INVARIANT: every stop source needs a matching acknowledgment, because an
    // unacknowledged stop keeps admission closed forever — recovering() stays
    // true and every later acquire() is refused Recovering. Today the only
    // production stop source is reset(), and teardownBackend()/onDisconnected()
    // acknowledge it, so the barrier always clears with the session. cancel(),
    // revoke(), expire() and emergencyStop() have no production callers yet;
    // whichever increment gives one of them a caller has to land its
    // acknowledgment path in the same change, not after it. RadioModel logs a
    // warning when it hits this refusal outside a disconnect gap.
    [[nodiscard]] bool acknowledgeStopped(const Operation& operation);
    [[nodiscard]] bool owns(const Actor& actor, const Operation& operation) const;
    [[nodiscard]] bool recovering() const;
    [[nodiscard]] bool hasInFlightDispatches() const;

private:
    struct Identity {
        static constexpr quint64 kChangingGeneration = quint64{1} << 63;
        std::atomic<quint64> generation{0};
        std::atomic<quint64> session{0};
        std::atomic<quint64> dispatches{0};
        // RX microphone media cannot hold up a fresh PTT intent. Teardown,
        // unlike operation admission, must account for these writes too.
        std::atomic<quint64> continuousDispatches{0};
        std::atomic<bool> alive{true};
    };
    struct ActorState {
        std::weak_ptr<Identity> coordinator;
        ActorPolicy policy;
        bool revoked{false};
    };
    struct ProducerState {
        std::weak_ptr<Identity> coordinator;
        std::atomic<bool> valid{true};
        bool continuousMicrophone{false};
    };
    struct OperationState {
        std::shared_ptr<ActorState> actor;
        std::atomic<bool> cancelled{false};
        qint64 startedMs{0};
        qint64 maximumMs{0};
        quint64 generation{0};
    };
    struct IntentState {
        Operation operation;
        Activity activity{Activity::Mox};
        std::atomic<bool> ended{false};
        bool finishing{false}; // owner-thread only; not the worker dispatch fence
    };

    [[nodiscard]] bool onThread() const;
    [[nodiscard]] bool validActor(const Actor& actor) const;
    void stop(StopReason reason);
    void endIntents(const Operation& operation);

    QThread* const m_thread;
    std::shared_ptr<Identity> m_identity;
    std::vector<std::weak_ptr<ActorState>> m_actors;
    std::vector<std::weak_ptr<ProducerState>> m_producers;
    std::vector<std::shared_ptr<IntentState>> m_intents;
    Operation m_active;
    Operation m_unconfirmed;
    Operation m_stopping;
    StopHandler m_stopHandler;
    bool m_inStopHandler{false};
};

} // namespace AetherSDR

Q_DECLARE_METATYPE(AetherSDR::TxCoordinator::Context)
