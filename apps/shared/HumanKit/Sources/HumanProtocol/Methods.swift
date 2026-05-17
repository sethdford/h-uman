import Foundation

/// RPC method name constants for the Human gateway control protocol.
///
/// Each constant is the wire string a `ControlRequest.method` must use to
/// invoke the named operation. Grouped by area for readability; `all`
/// returns the flat list.
@available(macOS 14.0, iOS 17.0, *)
public enum Methods {
    /// Open the control session; expects a `HelloOk` in reply.
    public static let connect = "connect"
    /// Liveness probe; returns server uptime and version metadata.
    public static let health = "health"
    /// Fetch the active runtime configuration.
    public static let configGet = "config.get"
    /// Fetch the JSON schema describing valid config shapes.
    public static let configSchema = "config.schema"
    /// Report server capabilities (providers, channels, tools).
    public static let capabilities = "capabilities"
    /// Send a chat turn; streams events back via `ControlEvent`.
    public static let chatSend = "chat.send"
    /// Fetch recent chat history for a session.
    public static let chatHistory = "chat.history"
    /// Abort an in-flight `chat.send`.
    public static let chatAbort = "chat.abort"
    /// Update one or more configuration keys.
    public static let configSet = "config.set"
    /// Apply staged config changes (may restart subsystems).
    public static let configApply = "config.apply"
    /// List known chat sessions.
    public static let sessionsList = "sessions.list"
    /// Mutate metadata on a chat session.
    public static let sessionsPatch = "sessions.patch"
    /// Delete a chat session.
    public static let sessionsDelete = "sessions.delete"
    /// List tools available to the agent.
    public static let toolsCatalog = "tools.catalog"
    /// Report per-channel status (paired, connected, queued).
    public static let channelsStatus = "channels.status"
    /// List configured cron entries.
    public static let cronList = "cron.list"
    /// Add a new cron entry.
    public static let cronAdd = "cron.add"
    /// Remove an existing cron entry.
    public static let cronRemove = "cron.remove"
    /// Trigger a cron entry immediately, out-of-band.
    public static let cronRun = "cron.run"
    /// List installed and available skills.
    public static let skillsList = "skills.list"
    /// Enable an installed skill.
    public static let skillsEnable = "skills.enable"
    /// Disable an installed skill without uninstalling it.
    public static let skillsDisable = "skills.disable"
    /// Probe whether a newer runtime build is available.
    public static let updateCheck = "update.check"
    /// Apply a pending runtime update.
    public static let updateRun = "update.run"
    /// Resolve a pending tool-execution approval (allow / deny).
    public static let execApprovalResolve = "exec.approval.resolve"
    /// Summarize token, dollar, and request usage over a window.
    public static let usageSummary = "usage.summary"

    // Activity & agents
    /// Stream the recent activity log (chat turns, tool calls, events).
    public static let activityRecent = "activity.recent"
    /// List available agent profiles.
    public static let agentsList = "agents.list"
    /// List models advertised by configured providers.
    public static let modelsList = "models.list"

    // Cron (additional)
    /// List historical runs for a given cron entry.
    public static let cronRuns = "cron.runs"
    /// Update an existing cron entry in place.
    public static let cronUpdate = "cron.update"

    // Skills (additional)
    /// Search the remote skill registry by keyword.
    public static let skillsSearch = "skills.search"
    /// Install a skill from the registry.
    public static let skillsInstall = "skills.install"
    /// Uninstall a previously-installed skill.
    public static let skillsUninstall = "skills.uninstall"
    /// Update an installed skill to a newer revision.
    public static let skillsUpdate = "skills.update"

    // Metrics & nodes
    /// Snapshot current runtime metrics (token rate, queue depth, RSS).
    public static let metricsSnapshot = "metrics.snapshot"
    /// Report SOTA benchmark scores for the running fleet.
    public static let sotaMetrics = "sota.metrics"
    /// Summarize security chain-of-thought entries over a window.
    public static let securityCotSummary = "security.cot.summary"
    /// List federation nodes the runtime is aware of.
    public static let nodesList = "nodes.list"
    /// Apply an action (pair, unpair, ping) to a federation node.
    public static let nodesAction = "nodes.action"

    // Voice & persona
    /// Transcribe a one-shot audio clip.
    public static let voiceTranscribe = "voice.transcribe"
    /// Clone a voice from a reference audio sample.
    public static let voiceClone = "voice.clone"
    /// Begin a streaming voice session.
    public static let voiceSessionStart = "voice.session.start"
    /// End a streaming voice session.
    public static let voiceSessionStop = "voice.session.stop"
    /// Interrupt the model's spoken reply mid-utterance.
    public static let voiceSessionInterrupt = "voice.session.interrupt"
    /// Signal end-of-audio for a streaming voice turn.
    public static let voiceAudioEnd = "voice.audio.end"
    /// Configure voice-session parameters (voice, format, language).
    public static let voiceConfig = "voice.config"
    /// Switch the active persona profile.
    public static let personaSet = "persona.set"

    // Auth (OAuth)
    /// Exchange a bearer token for a server-side session.
    public static let authToken = "auth.token"
    /// Start an OAuth authorization-code flow.
    public static let authOauthStart = "auth.oauth.start"
    /// Complete an OAuth authorization-code flow.
    public static let authOauthCallback = "auth.oauth.callback"
    /// Refresh an OAuth access token.
    public static let authOauthRefresh = "auth.oauth.refresh"

    // Memory (P0 native parity: list/recall/status for dashboard Memory view)
    /// Report memory subsystem health and capacity.
    public static let memoryStatus = "memory.status"
    /// List stored memory rows (paginated).
    public static let memoryList = "memory.list"
    /// Recall memories matching a query.
    public static let memoryRecall = "memory.recall"
    /// Store a new memory row.
    public static let memoryStore = "memory.store"
    /// Forget (delete) a memory row.
    public static let memoryForget = "memory.forget"
    /// Ingest a batch of memory rows from an external source.
    public static let memoryIngest = "memory.ingest"
    /// Run a memory consolidation pass.
    public static let memoryConsolidate = "memory.consolidate"

    // HuLa
    /// Aggregate analytics across HuLa traces over a window.
    public static let hulaTracesAnalytics = "hula.traces.analytics"
    /// List recent HuLa execution traces.
    public static let hulaTracesList = "hula.traces.list"
    /// Fetch a single HuLa trace by ID.
    public static let hulaTracesGet = "hula.traces.get"

    // Push notifications
    /// Register an APNs/FCM token for push delivery.
    public static let pushRegister = "push.register"
    /// Unregister a previously-registered push token.
    public static let pushUnregister = "push.unregister"

    /// All supported method names.
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
