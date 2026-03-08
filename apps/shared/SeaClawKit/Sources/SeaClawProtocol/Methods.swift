import Foundation

/// RPC method name constants for the SeaClaw gateway control protocol.
public enum Methods {
    public static let connect = "connect"
    public static let health = "health"
    public static let configGet = "config.get"
    public static let configSchema = "config.schema"
    public static let capabilities = "capabilities"
    public static let chatSend = "chat.send"
    public static let chatHistory = "chat.history"
    public static let chatAbort = "chat.abort"
    public static let configSet = "config.set"
    public static let configApply = "config.apply"
    public static let sessionsList = "sessions.list"
    public static let sessionsPatch = "sessions.patch"
    public static let sessionsDelete = "sessions.delete"
    public static let toolsCatalog = "tools.catalog"
    public static let channelsStatus = "channels.status"
    public static let cronList = "cron.list"
    public static let cronAdd = "cron.add"
    public static let cronRemove = "cron.remove"
    public static let cronRun = "cron.run"
    public static let skillsList = "skills.list"
    public static let skillsEnable = "skills.enable"
    public static let skillsDisable = "skills.disable"
    public static let updateCheck = "update.check"
    public static let updateRun = "update.run"
    public static let execApprovalResolve = "exec.approval.resolve"
    public static let usageSummary = "usage.summary"

    // Activity & agents
    public static let activityRecent = "activity.recent"
    public static let agentsList = "agents.list"
    public static let modelsList = "models.list"

    // Cron (additional)
    public static let cronRuns = "cron.runs"
    public static let cronUpdate = "cron.update"

    // Skills (additional)
    public static let skillsSearch = "skills.search"
    public static let skillsInstall = "skills.install"
    public static let skillsUninstall = "skills.uninstall"
    public static let skillsUpdate = "skills.update"

    // Metrics & nodes
    public static let metricsSnapshot = "metrics.snapshot"
    public static let nodesList = "nodes.list"
    public static let nodesAction = "nodes.action"

    // Voice & persona
    public static let voiceTranscribe = "voice.transcribe"
    public static let personaSet = "persona.set"

    // Auth (OAuth)
    public static let authToken = "auth.token"
    public static let authOauthStart = "auth.oauth.start"
    public static let authOauthCallback = "auth.oauth.callback"
    public static let authOauthRefresh = "auth.oauth.refresh"

    // Memory
    public static let memoryStatus = "memory.status"
    public static let memoryList = "memory.list"
    public static let memoryRecall = "memory.recall"
    public static let memoryStore = "memory.store"
    public static let memoryForget = "memory.forget"
    public static let memoryIngest = "memory.ingest"
    public static let memoryConsolidate = "memory.consolidate"

    // Push notifications
    public static let pushRegister = "push.register"
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
        metricsSnapshot, nodesList, nodesAction,
        voiceTranscribe, personaSet,
        authToken, authOauthStart, authOauthCallback, authOauthRefresh,
        memoryStatus, memoryList, memoryRecall, memoryStore, memoryForget, memoryIngest, memoryConsolidate,
        pushRegister, pushUnregister
    ]
}
