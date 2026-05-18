import Foundation

/// RPC method name constants for the Human gateway control protocol.
///
/// All values are stable wire identifiers. Add new methods by appending
/// to this namespace AND to the `all` array — the latter is consumed by
/// the gateway's allowlist check.
@available(macOS 14.0, iOS 17.0, *)
public enum Methods {
    /// RPC: `"connect"` — initial handshake; first frame the client sends.
    public static let connect = "connect"
    /// RPC: `"health"` — gateway health probe.
    public static let health = "health"
    /// RPC: `"config.get"` — read the user's configuration document.
    public static let configGet = "config.get"
    /// RPC: `"config.schema"` — fetch the JSON Schema for the config doc.
    public static let configSchema = "config.schema"
    /// RPC: `"capabilities"` — list features the gateway exposes.
    public static let capabilities = "capabilities"
    /// RPC: `"chat.send"` — send a chat message; returns assistant text.
    public static let chatSend = "chat.send"
    /// RPC: `"chat.history"` — page through prior conversation turns.
    public static let chatHistory = "chat.history"
    /// RPC: `"chat.abort"` — cancel an in-flight chat generation.
    public static let chatAbort = "chat.abort"
    /// RPC: `"config.set"` — write a single config field.
    public static let configSet = "config.set"
    /// RPC: `"config.apply"` — apply a multi-field config delta atomically.
    public static let configApply = "config.apply"
    /// RPC: `"sessions.list"` — list known chat sessions.
    public static let sessionsList = "sessions.list"
    /// RPC: `"sessions.patch"` — rename or relabel a session.
    public static let sessionsPatch = "sessions.patch"
    /// RPC: `"sessions.delete"` — permanently remove a session.
    public static let sessionsDelete = "sessions.delete"
    /// RPC: `"tools.catalog"` — list tools available to the agent.
    public static let toolsCatalog = "tools.catalog"
    /// RPC: `"channels.status"` — per-channel reachability snapshot.
    public static let channelsStatus = "channels.status"
    /// RPC: `"cron.list"` — list configured scheduled tasks.
    public static let cronList = "cron.list"
    /// RPC: `"cron.add"` — create a new scheduled task.
    public static let cronAdd = "cron.add"
    /// RPC: `"cron.remove"` — remove a scheduled task by id.
    public static let cronRemove = "cron.remove"
    /// RPC: `"cron.run"` — immediately fire a scheduled task.
    public static let cronRun = "cron.run"
    /// RPC: `"skills.list"` — list locally installed skills.
    public static let skillsList = "skills.list"
    /// RPC: `"skills.enable"` — enable a previously disabled skill.
    public static let skillsEnable = "skills.enable"
    /// RPC: `"skills.disable"` — disable a skill without uninstalling it.
    public static let skillsDisable = "skills.disable"
    /// RPC: `"update.check"` — probe for binary updates.
    public static let updateCheck = "update.check"
    /// RPC: `"update.run"` — apply a previously detected update.
    public static let updateRun = "update.run"
    /// RPC: `"exec.approval.resolve"` — approve or deny a pending tool exec.
    public static let execApprovalResolve = "exec.approval.resolve"
    /// RPC: `"usage.summary"` — token / cost rollup for billing surfaces.
    public static let usageSummary = "usage.summary"

    // MARK: - Activity & agents

    /// RPC: `"activity.recent"` — recent agent activity feed.
    public static let activityRecent = "activity.recent"
    /// RPC: `"agents.list"` — list configured agents.
    public static let agentsList = "agents.list"
    /// RPC: `"models.list"` — list configured AI models.
    public static let modelsList = "models.list"

    // MARK: - Cron (additional)

    /// RPC: `"cron.runs"` — paginated history of cron task invocations.
    public static let cronRuns = "cron.runs"
    /// RPC: `"cron.update"` — modify an existing cron task's schedule or prompt.
    public static let cronUpdate = "cron.update"

    // MARK: - Skills (additional)

    /// RPC: `"skills.search"` — search the skill registry.
    public static let skillsSearch = "skills.search"
    /// RPC: `"skills.install"` — install a skill from the registry.
    public static let skillsInstall = "skills.install"
    /// RPC: `"skills.uninstall"` — remove an installed skill.
    public static let skillsUninstall = "skills.uninstall"
    /// RPC: `"skills.update"` — update an installed skill to its latest version.
    public static let skillsUpdate = "skills.update"

    // MARK: - Metrics & nodes

    /// RPC: `"metrics.snapshot"` — point-in-time metrics snapshot.
    public static let metricsSnapshot = "metrics.snapshot"
    /// RPC: `"sota.metrics"` — SOTA scorecard metrics for the dashboard.
    public static let sotaMetrics = "sota.metrics"
    /// RPC: `"security.cot.summary"` — chain-of-thought security summary.
    public static let securityCotSummary = "security.cot.summary"
    /// RPC: `"nodes.list"` — list known peer / federation nodes.
    public static let nodesList = "nodes.list"
    /// RPC: `"nodes.action"` — admin action against a specific node.
    public static let nodesAction = "nodes.action"

    // MARK: - Voice & persona

    /// RPC: `"voice.transcribe"` — speech-to-text transcription request.
    public static let voiceTranscribe = "voice.transcribe"
    /// RPC: `"voice.clone"` — request a synthetic-voice clone job.
    public static let voiceClone = "voice.clone"
    /// RPC: `"voice.session.start"` — open a streaming voice session.
    public static let voiceSessionStart = "voice.session.start"
    /// RPC: `"voice.session.stop"` — close a streaming voice session.
    public static let voiceSessionStop = "voice.session.stop"
    /// RPC: `"voice.session.interrupt"` — barge-in / interrupt current speech.
    public static let voiceSessionInterrupt = "voice.session.interrupt"
    /// RPC: `"voice.audio.end"` — signal end of user audio in a session.
    public static let voiceAudioEnd = "voice.audio.end"
    /// RPC: `"voice.config"` — read or write the active voice configuration.
    public static let voiceConfig = "voice.config"
    /// RPC: `"persona.set"` — switch the active persona.
    public static let personaSet = "persona.set"

    // MARK: - Auth (OAuth)

    /// RPC: `"auth.token"` — exchange credentials for a session token.
    public static let authToken = "auth.token"
    /// RPC: `"auth.oauth.start"` — begin an OAuth provider authorization flow.
    public static let authOauthStart = "auth.oauth.start"
    /// RPC: `"auth.oauth.callback"` — complete an in-flight OAuth flow.
    public static let authOauthCallback = "auth.oauth.callback"
    /// RPC: `"auth.oauth.refresh"` — refresh a stored OAuth access token.
    public static let authOauthRefresh = "auth.oauth.refresh"

    // MARK: - Memory (P0 native parity: list/recall/status for dashboard Memory view)

    /// RPC: `"memory.status"` — high-level memory subsystem status.
    public static let memoryStatus = "memory.status"
    /// RPC: `"memory.list"` — paginated list of stored memories.
    public static let memoryList = "memory.list"
    /// RPC: `"memory.recall"` — retrieve memories matching a query.
    public static let memoryRecall = "memory.recall"
    /// RPC: `"memory.store"` — write a new memory entry.
    public static let memoryStore = "memory.store"
    /// RPC: `"memory.forget"` — delete a specific memory by id.
    public static let memoryForget = "memory.forget"
    /// RPC: `"memory.ingest"` — bulk-ingest external content.
    public static let memoryIngest = "memory.ingest"
    /// RPC: `"memory.consolidate"` — run a memory consolidation pass.
    public static let memoryConsolidate = "memory.consolidate"

    // MARK: - HuLa

    /// RPC: `"hula.traces.analytics"` — HuLa trace aggregate analytics.
    public static let hulaTracesAnalytics = "hula.traces.analytics"
    /// RPC: `"hula.traces.list"` — paginated list of HuLa traces.
    public static let hulaTracesList = "hula.traces.list"
    /// RPC: `"hula.traces.get"` — fetch a single HuLa trace by id.
    public static let hulaTracesGet = "hula.traces.get"

    // MARK: - Push notifications

    /// RPC: `"push.register"` — register a device push token with the gateway.
    public static let pushRegister = "push.register"
    /// RPC: `"push.unregister"` — unregister a device push token.
    public static let pushUnregister = "push.unregister"

    /// All supported method names. Used by the gateway allowlist.
    public static let all: [String] = [
        connect, health, configGet, configSchema, capabilities,
        chatSend, chatHistory, chatAbort,
        configSet, configApply, sessionsList, sessionsPatch, sessionsDelete,
        toolsCatalog, channelsStatus,
        cronList, cronAdd, cronRemove, cronRun, cronRuns, cronUpdate,
        skillsList, skillsEnable, skillsDisable, skillsSearch, skillsInstall, skillsUninstall, skillsUpdate,
        updateCheck, updateRun,
        execApprovalResolve, usageSummary,
        activityRecent, agentsList, modelsList,
        metricsSnapshot, sotaMetrics, securityCotSummary, nodesList, nodesAction,
        voiceTranscribe, voiceClone, voiceSessionStart, voiceSessionStop, voiceSessionInterrupt, voiceAudioEnd, voiceConfig, personaSet,
        authToken, authOauthStart, authOauthCallback, authOauthRefresh,
        memoryStatus, memoryList, memoryRecall, memoryStore, memoryForget, memoryIngest, memoryConsolidate,
        hulaTracesAnalytics, hulaTracesList, hulaTracesGet,
        pushRegister, pushUnregister
    ]
}
