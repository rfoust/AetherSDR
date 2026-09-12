#include "RadioModel.h"
#include "core/LogManager.h"

#include <QPointer>

namespace AetherSDR {

qint64 RadioModel::txMonotonicMs()
{
    return TxCoordinator::monotonicMs();
}

TxCoordinator::Producer RadioModel::registerTxProducer(QObject* lifetime, bool continuousMicrophone)
{
    if (!lifetime || QThread::currentThread() != thread()) {
        return {};
    }
    const TxCoordinator::Producer producer = m_txCoordinator.registerProducer(continuousMicrophone);
    connect(lifetime, &QObject::destroyed, this, [producer] {
        producer.invalidate();
    }, Qt::DirectConnection); // atomic invalidation; never touches a model
    return producer;
}

TxCoordinator::Producer RadioModel::registerTxProducer()
{
    return QThread::currentThread() == thread() ? m_txCoordinator.registerProducer() : TxCoordinator::Producer{};
}

TxCoordinator::Context RadioModel::captureTxMedia(const TxCoordinator::Request& request) const
{
    if (QThread::currentThread() != thread() || m_txSessionClosing || !m_backend) {
        return {};
    }
    return m_txCoordinator.mediaContext(request);
}

TxCoordinator::Context RadioModel::captureTxMedia(const TxCoordinator::Producer& producer) const
{
    if (QThread::currentThread() != thread() || m_txSessionClosing || !m_backend) {
        return {};
    }
    return m_txCoordinator.mediaContext(producer, m_txOperation);
}

bool RadioModel::beginLocalTxActivity(TxActivity activity)
{
    return beginTxActivity(activity, nullptr);
}

bool RadioModel::requestProducerPttOn(const TxCoordinator::Request& request,
                                     TransmitModel::PttSource source)
{
    if (QThread::currentThread() != thread()) {
        return false;
    }
    bool engaged = false;
    m_transmitModel.requestPttOn(source, [this, request]() -> TransmitModel::KeyingPermit {
        if (!beginTxActivity(TxActivity::Mox, &request)) {
            return {};
        }
        const TxCoordinator::Operation operation = m_txCoordinator.requestOperation(request);
        return [operation] { return operation.permitsDispatch(txMonotonicMs()); };
    }, [this, request, source, &engaged] {
        engaged = setProducerTransmit(request, true, source);
    });
    if (!engaged) {
        // Preflight/admission can reenter. A refusal is not a held request
        // that a later callback may opportunistically turn into transmit.
        (void)setProducerTransmit(request, false, source);
    }
    return engaged;
}

void RadioModel::requestProducerPttOff(const TxCoordinator::Request& request,
                                      TransmitModel::PttSource source)
{
    if (QThread::currentThread() != thread()) {
        return;
    }
    const TxCoordinator::Intent intent = m_txCoordinator.closeRequest(request);
    if (!intent.pending()) {
        return;
    }
    const TxCoordinator::Operation operation = m_txCoordinator.requestOperation(request);
    if (m_txCoordinator.hasOtherIntents(operation, intent)) {
        endLocalTxActivity(intent);
        m_txRequested = activeTxActivities() & static_cast<unsigned>(TxActivity::Mox);
        return;
    }
    const QPointer<RadioModel> receiver(this);
    m_transmitModel.requestPttOff(source, {
        [operation, intent] { return operation.permitsCleanup() && intent.pending(); },
        [receiver, request, source] {
            if (receiver) {
                receiver->setTransmitImpl(false, source, &request, true);
            }
        },
        [receiver, intent] {
            if (receiver) {
                receiver->endLocalTxActivity(intent);
            }
        }});
}

void RadioModel::abortProducerPtt(const TxCoordinator::Request& request,
                                 TransmitModel::PttSource source)
{
    if (QThread::currentThread() != thread()) {
        return;
    }
    (void)m_txCoordinator.closeRequest(request);
    const TxCoordinator::Intent intent = m_txCoordinator.requestIntent(request);
    if (intent.pending()) {
        (void)setTransmitImpl(false, source, &request, true);
        return;
    }
    const TxCoordinator::Operation operation = m_txCoordinator.requestOperation(request);
    if (operation.permitsCleanup() && !m_txCoordinator.hasOtherIntents(operation, {})) {
        // Retry only this operation's one-way stop after a late radio edge.
        // A new contributor or connection can never inherit this cleanup.
        requestTransmitStop(operation);
    }
}

bool RadioModel::beginTxActivity(TxActivity activity, const TxCoordinator::Request* request)
{
    if (request && !m_txCoordinator.acceptsRequest(*request)) {
        return false;
    }
    if (m_txSessionClosing) {
        emitInterlockNotification(tr("Transmit is unavailable while the radio disconnects."),
                                  QStringLiteral("tx-session-closing"));
        return false;
    }
    if (!m_backend || !refuseKeyOnTransmitIncapableBackend()
        || !refuseKeyInReceiveOnlyMode()) {
        return false;
    }
    const RadioCapabilities caps = backendCapabilities();
    if ((activity == TxActivity::Cwx && !caps.hasRadioSideCwKeyer)
        || (activity == TxActivity::Atu && !caps.hasTuner)) {
        emitInterlockNotification(tr("This radio does not support the requested transmit operation."),
                                  QStringLiteral("tx-operation-unsupported"));
        return false;
    }
    const QString gate = activity == TxActivity::Tune || activity == TxActivity::Atu
        ? QStringLiteral("tune-start")
        : activity == TxActivity::Mox ? QStringLiteral("xmit") : QStringLiteral("cw-key");
    if (transmitStartBlockedByInhibit(gate)) {
        return false;
    }
    if (!m_backendTxProducer.valid()) {
        m_backendTxProducer = registerTxProducer(m_backend.get());
    }
    if (!m_backendTxProducer.valid()) {
        emitInterlockNotification(tr("Transmit producer capacity is exhausted."),
                                  QStringLiteral("tx-producer-capacity"));
        return false;
    }
    const TxCoordinator::Admission admission = m_txCoordinator.acquire(m_desktopTxActor, txMonotonicMs());
    if (!admission.accepted()) {
        // One message per reason. Only Recovering is reachable while a single
        // desktop actor exists, but the others become reachable as soon as a
        // per-client actor does, and "cleanup is in progress" would then be a
        // wrong explanation rather than a vague one.
        QString message;
        QString key;
        switch (admission.refusal) {
        case TxCoordinator::Refusal::Recovering:
            message = tr("Transmit cleanup is still in progress.");
            key = QStringLiteral("tx-coordinator-recovering");
            break;
        case TxCoordinator::Refusal::Busy:
            message = tr("Another client is transmitting.");
            key = QStringLiteral("tx-coordinator-busy");
            break;
        case TxCoordinator::Refusal::Denied:
            message = tr("This client is not permitted to transmit.");
            key = QStringLiteral("tx-coordinator-denied");
            break;
        case TxCoordinator::Refusal::InvalidActor:
        case TxCoordinator::Refusal::WrongThread:
        case TxCoordinator::Refusal::None:
            message = tr("Transmit is unavailable.");
            key = QStringLiteral("tx-coordinator-unavailable");
            break;
        }
        // m_txSessionClosing short-circuits above, so a Recovering refusal here
        // is a stop that was never acknowledged rather than a normal disconnect
        // gap. That is a permanent admission latch, so say so instead of
        // leaving it to be diagnosed from a silent refusal. See
        // TxCoordinator::acknowledgeStopped().
        if (admission.refusal == TxCoordinator::Refusal::Recovering) {
            if (m_txCoordinator.hasInFlightDispatches()) {
                qCWarning(lcProtocol) << "RadioModel: TX refused — preceding terminal writer still entered;"
                                     << "retry requires fresh intent after it returns";
            } else {
                qCWarning(lcProtocol)
                    << "RadioModel: TX refused — coordinator stop is unacknowledged;"
                    << "admission stays closed until the session ends";
            }
        }
        emitInterlockNotification(message, key);
        return false;
    }
    if (!m_txOperation.sameOperation(admission.operation)) {
        m_pendingTxDeliveries = 0;
        m_txOperationActivities = 0;
    }
    m_txOperation = admission.operation;
    const TxCoordinator::Intent intent = request
        ? m_txCoordinator.beginRequest(*request, m_txOperation, activity)
        : m_txCoordinator.beginIntent(m_txOperation, m_localTxIntents.value(activity), activity);
    if (!intent.pending()) {
        qCWarning(lcProtocol) << "RadioModel: TX producer intent could not be registered";
        completeLocalTxIfDrained();
        return false;
    }
    if (!request) {
        m_localTxIntents.insert(activity, intent);
    }
    m_txOperationActivities |= static_cast<unsigned>(activity);
    m_backend->setTransmitContext(m_txCoordinator.mediaContext(m_backendTxProducer,
        request ? m_txCoordinator.requestOperation(*request) : m_txOperation));
    return true;
}

void RadioModel::endLocalTxActivity(const TxCoordinator::Intent& intent)
{
    if (m_txCoordinator.endIntent(intent)) {
        completeLocalTxIfDrained();
    }
}

unsigned RadioModel::activeTxActivities() const
{
    // Include older draining contributions, not just the current compatibility
    // slot. A later completed edge cannot hide an earlier pending CW tail.
    return m_txCoordinator.activeActivities(m_txOperation);
}

void RadioModel::completeLocalTxIfDrained()
{
    if (!m_txCoordinator.hasIntents(m_txOperation) && m_pendingTxDeliveries == 0) {
        // Existing desktop sequencers explicitly end their local intent. This
        // fences pending work; it is NOT a claim that the radio is observed RX.
        // The coordinator retains this actor's ownership until qualified
        // acknowledgment; only this same compatibility actor can reengage.
        (void)m_txCoordinator.finishLocalIntent(m_txOperation);
    }
}

void RadioModel::acknowledgeTxTransportTeardown(const TxCoordinator::Operation& operation)
{
    // The lifecycle caller has already established transport teardown. An
    // entered writer can still be returning through a reentrant disconnect;
    // retry that bookkeeping only. This timer never supplies radio-stop proof.
    if (!operation.sameOperation(m_txOperation)
        || m_txCoordinator.acknowledgeStopped(operation)
        || !m_txCoordinator.recovering() || !m_txCoordinator.hasInFlightDispatches()) {
        return;
    }
    QTimer::singleShot(10, this, [this, operation] {
        acknowledgeTxTransportTeardown(operation);
    });
}

std::function<void()> RadioModel::trackTxDelivery(const TxCoordinator::Operation& operation)
{
    const bool tracked = operation.sameOperation(m_txOperation) && operation.permitsCleanup();
    if (tracked) {
        ++m_pendingTxDeliveries;
    }
    return [this, operation, tracked] {
        if (!tracked || !m_txOperation.sameOperation(operation)
            || m_pendingTxDeliveries == 0) {
            return;
        }
        --m_pendingTxDeliveries;
        completeLocalTxIfDrained();
    };
}

TxCoordinator::Completion RadioModel::trackTxQueue(const TxCoordinator::Operation& operation,
                                                   std::function<void()> finished)
{
    const QPointer<RadioModel> receiver(this);
    const auto consumed = trackTxDelivery(operation);
    const auto finish = [receiver, consumed, finished] {
        if (receiver) {
            consumed();
        }
        if (receiver && finished) {
            finished();
        }
    };
    return TxCoordinator::Completion([receiver, finish] {
        if (receiver) {
            if (QThread::currentThread() == receiver->thread()) {
                finish();
            } else {
                QMetaObject::invokeMethod(receiver, finish, Qt::QueuedConnection);
            }
        }
    });
}

void RadioModel::sendTxKeyingCommand(const QString& command, const TxCoordinator::Command& fence)
{
    const TxCoordinator::Operation operation = fence.operation;
    const auto completed = [completion = fence.completion] { completion.finish(); };
    if (fence.keying) {
        if (!sendTxTcpCommand(command, operation, true, completed)) {
            completed();
        }
        return;
    }
    // Retain a short, normally released operation until its queued key-up is
    // consumed, just as NetCW does. Otherwise completion cancels an earlier
    // key-on before the transport has had a chance to consume either edge.
    // This is local queue completion, not qualified radio-idle evidence.
    if (!sendTxTcpCommand(command, operation, false, completed)) {
        completed();
    }
}

void RadioModel::sendCwxCommand(const QString& command, bool keying, ResponseCallback reply)
{
    const TxCoordinator::Operation operation = m_txOperation;
    const TxCoordinator::Operation fence = keying || operation.permitsCleanup()
        ? operation : m_txCoordinator.cleanupFence();
    const auto consumed = trackTxDelivery(operation);
    // An ESC must cancel queued text even while a separate MOX intent keeps
    // the shared desktop operation alive. Never read CwxModel on the worker.
    const auto batch = m_cwxModel.queuedTransmissionPermit();
    if (!sendTxTcpCommand(command, fence, keying, consumed, std::move(reply), batch)) {
        consumed();
    }
}

void RadioModel::stopTxOperation(const TxCoordinator::Operation& operation,
                               TxCoordinator::StopReason reason)
{
    Q_UNUSED(reason);
    // TxCoordinator has already invalidated the keying fence and entered
    // recovery. All cleanup below is key-up/bypass/abort; never re-admit it.
    requestTransmitStop(operation);
    m_localTxIntents.clear();
    m_txOperation = operation;
    // No acknowledgment here: queued stop commands are not stopped-radio
    // evidence. The lifecycle caller acknowledges only after transport loss.
}

void RadioModel::requestTransmitStop(const TxCoordinator::Operation& operation)
{
    if (QThread::currentThread() != thread()) {
        return;
    }
    const QPointer<RadioModel> radio(this);
    const auto current = [radio, operation] {
        return radio && operation.permitsCleanup()
            && operation.sameOperation(radio->m_txOperation);
    };
    if (!current()) {
        return;
    }
    const unsigned activities = activeTxActivities();
    const bool hadCwx = m_txOperationActivities & static_cast<unsigned>(TxActivity::Cwx);
    m_transmitModel.cancelPttRelease();
    if (current() && hadCwx) {
        m_cwxModel.clearBuffer();
    }
    if (current() && (activities & static_cast<unsigned>(TxActivity::CwKey))) {
        sendCwKey(false);
    }
    if (current() && (activities & static_cast<unsigned>(TxActivity::CwPtt))) {
        sendCwPtt(false);
    }
    if (current() && (activities & static_cast<unsigned>(TxActivity::Tune))) {
        m_transmitModel.stopTune();
    }
    if (current() && (activities & static_cast<unsigned>(TxActivity::Atu))) {
        m_transmitModel.atuBypass();
    }
    if (current()) {
        // Also close a reported tail after normal local handoff. Do not touch
        // ATU relay configuration unless this operation actually requested ATU.
        m_transmitModel.setMox(false);
    }
}

void RadioModel::resetTxOperations()
{
    // Even an idle coordinator must refuse new intent while the session dies.
    // Close admission BEFORE cancellation/reply/model notifications can reenter
    // us; operation recovery alone only covers a previously active operation.
    m_txSessionClosing = true;
    m_pendingTxDeliveries = 0;
    m_cwInputSession.fetch_add(1, std::memory_order_release);
    m_cwInputNotBefore = std::chrono::steady_clock::now();
    m_transmitModel.cancelPttRelease();
    m_txCoordinator.reset();
    m_cwxModel.resetDrainWatch();
    m_localTxIntents.clear();
}

void RadioModel::queueCwKeyEdge(bool down, const QString& source, quint64 traceId,
                              quint64 sourceMs, std::chrono::steady_clock::time_point scheduledAt)
{
    const quint64 session = m_cwInputSession.load(std::memory_order_acquire);
    QMetaObject::invokeMethod(this, [this, session, down, source, traceId, sourceMs, scheduledAt] {
        if (session != m_cwInputSession.load(std::memory_order_acquire)
            || m_txSessionClosing || scheduledAt < m_cwInputNotBefore) {
            return;
        }
        sendCwKeyEdge(down, source, traceId, sourceMs, scheduledAt);
    }, Qt::QueuedConnection);
}

} // namespace AetherSDR
