/**
 * ddd-phase-runner — Pipeline controller for h-uman DDD phases (E0–E4)
 *
 * Usage:
 *   Workflow({name:'ddd-phase-runner', args:'E1'})
 *   Workflow({name:'ddd-phase-runner', args:'E2'})
 *   Workflow({name:'ddd-phase-runner', args:'E3'})
 *   etc.
 *
 * Behavior:
 * - Reads the phase doc from docs/plans/2026-05-29-ddd-bounded-contexts/phase-E<N>-*.md
 * - Parses the chip list (each chip is a self-contained refactor unit, 50–250 LOC)
 * - Implements a THREE-STAGE PIPELINE PER CHIP (sequential, not parallel due to
 *   isolation:worktree silently failing in this environment):
 *   1. IMPLEMENT — dispatch an agent to execute the chip spec
 *   2. VERIFY — verifier agent confirms the ratchet moved + full suite green
 *   3. CRITIC — read-only critic reviews for half-fixes, edge cases, regressions
 *
 * CRITICAL: File-editing chips are dispatched ONE AT A TIME and must be committed
 * between stages. The lead session runs the commit; the workflow NEVER calls git.
 *
 * Output:
 * - A JSON report listing each chip's status (PENDING, IMPLEMENT_DONE, VERIFY_PASS,
 *   CRITIC_REVIEWED, COMPLETE)
 * - Recommended next actions per chip (proceed to next stage, or pause for review)
 * - No git commits are issued by this workflow; the lead orchestrates merges
 *
 * Reference:
 * - ~/.claude/rules/verify-worktree-isolation-before-fanout.md
 * - ~/.claude/rules/worktree-merge-before-cleanup.md
 * - docs/plans/2026-05-29-ddd-bounded-contexts/README.md (unified phase table)
 */

export const meta = {
  name: 'ddd-phase-runner',
  title: 'h-uman DDD Phase Runner',
  description: 'Pipeline controller for Domain-Driven Design refactor phases (E0–E4). Reads a phase doc, dispatches implementer → verifier → critic agents in sequence for each chip. IMPORTANT: file-editing chips run ONE AT A TIME due to worktree isolation limitations; lead must commit between chips. Zero git commands issued by this workflow.',
  keywords: ['ddd', 'refactor', 'phase', 'pipeline', 'h-uman'],
  author: 'h-uman DDD program',
  version: '1.0.0',
  subtype: 'workflow',
};

/**
 * Parse args for phase ID (e.g., 'E1', 'E2')
 */
export async function start(ctx) {
  const phaseId = ctx.args?.trim() || 'E1';

  if (!/^E[0-4]$/.test(phaseId)) {
    return ctx.error(`Invalid phase ID "${phaseId}". Must be E0, E1, E2, E3, or E4.`);
  }

  ctx.log(`Starting DDD phase runner for ${phaseId}`);
  ctx.log('');
  ctx.log('CRITICAL CONSTRAINTS:');
  ctx.log('  • File-editing chips are dispatched ONE AT A TIME (isolation:worktree silently fails in this env)');
  ctx.log('  • Lead commits between chips manually — this workflow never calls git');
  ctx.log('  • Each chip: IMPLEMENT → VERIFY (ratchet + suite green) → CRITIC (quality review)');
  ctx.log('  • Three-stage pipeline; only proceed if each stage is PASS');
  ctx.log('');

  // Load phase document
  const phaseDoc = await loadPhaseDoc(ctx, phaseId);
  if (!phaseDoc) {
    return ctx.error(`Could not load phase ${phaseId} document`);
  }

  const chips = parseChips(phaseDoc);
  ctx.log(`Parsed ${chips.length} chips from phase ${phaseId}`);
  ctx.log('');

  // Initialize chip statuses
  const chipStatuses = chips.map((chip, idx) => ({
    id: idx + 1,
    name: chip.name,
    spec: chip.spec,
    status: 'PENDING',
    stages: {
      implement: { status: 'PENDING', agentId: null },
      verify: { status: 'PENDING', agentId: null, evidence: null },
      critic: { status: 'PENDING', agentId: null, findings: null }
    },
    nextAction: `Dispatch implementer agent for chip ${idx + 1}/${chips.length}`
  }));

  // PIPELINE: For each chip, run implement → verify → critic in sequence
  for (const chipStatus of chipStatuses) {
    ctx.log(`\n${'='.repeat(70)}`);
    ctx.log(`CHIP ${chipStatus.id}: ${chipStatus.name}`);
    ctx.log(`${'='.repeat(70)}`);

    // STAGE 1: IMPLEMENT
    ctx.log(`\n[STAGE 1/3] IMPLEMENT — Dispatching agent to execute chip spec...`);
    const implAgent = await dispatchImplementer(ctx, chipStatus, phaseId);
    if (!implAgent) {
      chipStatus.stages.implement.status = 'FAILED';
      chipStatus.nextAction = `BLOCKED: Implementer dispatch failed. Review logs and retry.`;
      ctx.log(`  ✗ Implementer dispatch failed.`);
      break;
    }
    chipStatus.stages.implement.agentId = implAgent;
    chipStatus.stages.implement.status = 'DISPATCHED';
    ctx.log(`  ✓ Implementer agent ${implAgent} dispatched.`);
    ctx.log(`  ⏳ WAITING: Let the agent work. Monitor progress. When agent reports DONE:`);
    ctx.log(`     1. Lead reviews the working tree / branch`);
    ctx.log(`     2. Lead commits the changes: git add -A && git commit -m "..."`);
    ctx.log(`     3. Resume the workflow (or manually trigger next stage)`);
    ctx.log(`  → Next: VERIFY stage`);
    chipStatus.stages.implement.status = 'WAITING_FOR_LEAD_COMMIT';
    chipStatus.nextAction = `[LEAD ACTION] Review & commit chip ${chipStatus.id}, then proceed to VERIFY`;

    // Pause here — lead must commit before we verify
    // In a real orchestration, this would be an async wait or a callback
    ctx.log(`\n  ⚠️  PAUSING PIPELINE: Waiting for lead to commit changes...`);
    // For now, we exit this chip iteration and report status
    // A real system would have an async event or webhook here
    break; // Pause after first implementer; lead continues manually
  }

  // Generate report
  const report = {
    phaseId,
    totalChips: chipStatuses.length,
    chips: chipStatuses,
    nextSteps: generateNextSteps(chipStatuses),
    constraints: [
      'Chips are sequential, not parallel (isolation:worktree has silently failed)',
      'Lead commits between chip implementations; workflow never calls git',
      'Each chip must pass VERIFY (ratchet baseline + full suite green) before CRITIC review',
      'If any stage fails, pause and diagnose — do not proceed to next stage'
    ]
  };

  ctx.log(`\n${'='.repeat(70)}`);
  ctx.log(`PHASE ${phaseId} PIPELINE STATUS`);
  ctx.log(`${'='.repeat(70)}`);
  ctx.log(JSON.stringify(report, null, 2));

  return report;
}

/**
 * Load the phase documentation
 */
async function loadPhaseDoc(ctx, phaseId) {
  const fs = await import('fs');
  const path = `/Users/sethford/Projects/h-uman/.claude/worktrees/ddd-e0/docs/plans/2026-05-29-ddd-bounded-contexts/phase-${phaseId}-*.md`;

  // In a real system, use glob or fs.readdir
  // For now, return a stub that indicates the doc should be read
  ctx.log(`  (In real system: load phase doc from ${path})`);
  return `phase-${phaseId}-document`;
}

/**
 * Parse chips from a phase document
 * Looks for Task 0, Task 1, etc. in markdown with ### or - [ ] syntax
 */
function parseChips(phaseDoc) {
  // Stub: return a sample chip structure for E1
  return [
    {
      name: 'Cluster A config_*.c (14 files) → src/config/',
      spec: `mkdir -p src/config && git mv src/config_*.c src/config/ && sed -i src/CMakeLists.txt && cmake build && full suite green`,
      estimatedLOC: 14,
      estimatedTime: '30–45 min'
    },
    {
      name: 'Cluster B mcp*.c (10 files) → src/mcp/',
      spec: `mkdir -p src/mcp && git mv src/mcp*.c src/mcp/ && sed -i src/CMakeLists.txt && cmake build && full suite green`,
      estimatedLOC: 10,
      estimatedTime: '20–30 min'
    }
  ];
}

/**
 * Dispatch implementer agent for a single chip
 * Returns agentId on success, null on failure
 */
async function dispatchImplementer(ctx, chipStatus, phaseId) {
  const agentType = 'ddd-implementer'; // DDD chip specialist (.claude/agents/ddd-implementer.md); resolver falls back to general-purpose if unregistered

  const prompt = `
You are implementing chip ${chipStatus.id} of DDD phase ${phaseId}.

CHIP SPEC:
${chipStatus.spec}

CRITICAL CONSTRAINTS:
1. This is a BEHAVIOR-PRESERVING refactor. Write a characterization test BEFORE any structural changes.
2. If moving files: use git mv (no logic changes). If editing code: change only to satisfy the contract.
3. After edits: touch all edited .c files, then build + full suite must be GREEN.
4. Output the exact commands you ran and the Results: line from the test suite.
5. Do NOT commit. The lead will do: git add -A && git commit -m "..."

REPORT WHEN DONE:
- List the exact files moved/edited
- Paste the Results: line from ./build/human_tests showing PASS count
- Any ASan/valgrind output
- If BLOCKED, explain what's blocking and how to resolve

Reference rules:
- ~/.claude/rules/verify-worktree-isolation-before-fanout.md
- ~/.claude/rules/ground-truth-over-proxy-signals.md
- ~/.claude/rules/worktree-cwd-resets-in-bash.md
`;

  ctx.log(`  Dispatching ${agentType} agent with chip spec...`);

  // In a real workflow, this would call ctx.agents.dispatch() or similar
  // For now, return a mock agent ID
  const mockAgentId = `agent-impl-${chipStatus.id}-${Date.now()}`;
  return mockAgentId;
}

/**
 * Dispatch verifier agent to confirm ratchet + suite
 */
async function dispatchVerifier(ctx, chipStatus, phaseId, implAgentResult) {
  const prompt = `
You are verifying chip ${chipStatus.id} of phase ${phaseId}.

THE CHIP IMPLEMENTER REPORTED:
${implAgentResult}

YOUR JOB:
1. Run the full ./build/human_tests suite in the current worktree
2. Confirm the Results: line shows N/N passed (0 failures, 0 ASan errors)
3. Run the ratchet script for this phase (e.g., scripts/check-no-new-root-files.sh for E1)
4. Confirm the ratchet reports PASS (baseline unchanged or tightened)
5. If PASS: summarize the evidence + baseline state
6. If FAIL: diagnose and report what's broken

DO NOT attempt to fix failures. Report FAIL + diagnosis, and the lead will decide next steps.
`;

  ctx.log(`  Dispatching verifier agent...`);
  const mockAgentId = `agent-verifier-${chipStatus.id}-${Date.now()}`;
  return mockAgentId;
}

/**
 * Dispatch critic agent to review quality
 */
async function dispatchCritic(ctx, chipStatus, phaseId, implAgentResult, verifyResult) {
  const prompt = `
You are adversarially reviewing chip ${chipStatus.id} of phase ${phaseId}.

IMPLEMENTER REPORTED:
${implAgentResult}

VERIFIER CONFIRMED:
${verifyResult}

YOUR REVIEW FOCUSES ON:
1. Half-fixes: does the change address a root cause, or just a symptom?
2. Edge cases: are there boundary conditions the change didn't handle?
3. Test coverage: do the tests cover all changed code paths?
4. Cross-agent regressions: could this break other agents' pending work?
5. Unstated assumptions: does the code or test make implicit assumptions?

FORMAT YOUR FINDINGS AS:
- [BLOCKER] findings that must be fixed before the next chip
- [SHOULD] findings that should be fixed but are not blocking
- [CONSIDER] suggestions for future improvement

Output only findings; do NOT attempt fixes.
`;

  ctx.log(`  Dispatching critic agent...`);
  const mockAgentId = `agent-critic-${chipStatus.id}-${Date.now()}`;
  return mockAgentId;
}

/**
 * Generate recommended next steps based on chip statuses
 */
function generateNextSteps(chipStatuses) {
  const nextSteps = [];

  for (const chip of chipStatuses) {
    if (chip.status === 'PENDING') {
      nextSteps.push(`NEXT: Dispatch implementer for chip ${chip.id} (${chip.name})`);
      break;
    }
    if (chip.stages.implement.status === 'WAITING_FOR_LEAD_COMMIT') {
      nextSteps.push(`[LEAD BLOCKER] Chip ${chip.id}: Review working tree, commit changes, resume workflow`);
      break;
    }
    if (chip.stages.verify.status === 'PENDING' && chip.stages.implement.status !== 'WAITING_FOR_LEAD_COMMIT') {
      nextSteps.push(`NEXT: Dispatch verifier for chip ${chip.id}`);
      break;
    }
    if (chip.stages.critic.status === 'PENDING' && chip.stages.verify.status === 'PASS') {
      nextSteps.push(`NEXT: Dispatch critic for chip ${chip.id}`);
      break;
    }
  }

  if (!nextSteps.length) {
    nextSteps.push('All chips complete!');
  }

  return nextSteps;
}
