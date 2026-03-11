# SDLC Plugin - Complete Content

This document contains all files needed for the cross-role SDLC plugin. Copy this structure to `/Users/sethford/Documents/claude-skills/sdlc/`.

## Directory Structure

```
sdlc/
  .claude-plugin/
    plugin.json
  README.md
  skills/
    sdlc-phase-gate/
      SKILL.md
    definition-of-ready/
      SKILL.md
    definition-of-done/
      SKILL.md
    feature-lifecycle/
      SKILL.md
    release-checklist/
      SKILL.md
    incident-to-improvement/
      SKILL.md
    retrospective-to-action/
      SKILL.md
    compliance-traceability/
      SKILL.md
  commands/
    design-review.md
    feature-kickoff.md
    pre-launch-review.md
    security-review.md
    quality-gate.md
    full-lifecycle.md
    post-incident-review.md
    sprint-ceremony.md
```

---

## .claude-plugin/plugin.json

```json
{
  "name": "sdlc",
  "version": "1.0.0",
  "description": "Cross-role SDLC workflows that chain architect, engineer, PM, security, QA, designer, and tech lead skills into end-to-end lifecycle commands.",
  "author": { "name": "Seth Ford" },
  "keywords": [
    "sdlc",
    "lifecycle",
    "cross-functional",
    "workflow",
    "gates",
    "phase-gates",
    "definition-of-done",
    "definition-of-ready"
  ],
  "license": "MIT",
  "repository": {
    "type": "git",
    "url": "https://github.com/sethdford/claude-skills"
  }
}
```

---

## README.md

```markdown
# SDLC Plugin: Cross-Functional Lifecycle Workflows

A comprehensive plugin that chains skills across all engineering roles to create end-to-end SDLC workflows. This is the unique differentiator — no other skills collection orchestrates architect, engineer, PM, security, QA, designer, and tech-lead perspectives into unified lifecycle commands.

## Overview

Software development lifecycle (SDLC) is fundamentally cross-functional. Requirements flow through architecture to implementation; design decisions have security implications; code changes require QA validation. Yet most tools treat these as separate domains.

This plugin bridges that gap by:

1. **Chaining role-specific skills** into cohesive workflows
2. **Enforcing phase gates** that validate readiness before transitions
3. **Building traceability** from requirements through deployment
4. **Automating ceremony coordination** (kickoffs, retros, incident reviews)
5. **Grounding in ISO/IEC standards** (12207, 15288) for lifecycle process rigor

## Skills (8 core)

### sdlc-phase-gate

Validates that work meets criteria before moving between SDLC phases. Implements phase gates for: Requirements → Design → Implementation → Testing → Deployment → Operations. References ISO/IEC 12207 software lifecycle processes.

- **Input**: Current phase, work item, team context
- **Output**: Go/no-go decision with required changes
- **Chains with**: definition-of-ready, definition-of-done

### definition-of-ready

Ensures work items are ready for development before sprint intake. Combines PM requirements skills + architect design feasibility + security threat assessment.

- **Input**: Work item (story, feature, task)
- **Output**: DoR checklist with approval gates
- **Chains with**: feature-kickoff, sprint-ceremony

### definition-of-done

Validates completed work across all dimensions: code quality, test coverage, security review, documentation, accessibility. Implements the complete intersection of all roles' acceptance criteria.

- **Input**: Completed work item with artifacts
- **Output**: DoD checklist; approval blocks on failures
- **Chains with**: quality-gate, release-checklist

### feature-lifecycle

Maps the full lifecycle of a feature from idea through production monitoring. Explicitly shows each role's contribution at each phase. Produces a living document that evolves as the feature progresses.

- **Input**: Feature description, team structure
- **Output**: Feature lifecycle document with role responsibilities
- **Chains with**: full-lifecycle

### release-checklist

Pre-release validation across all roles: PM sign-off, QA test pass, security scan, performance baseline, rollback plan, monitoring setup. Produces a release readiness checklist with sign-off evidence.

- **Input**: Feature set, release notes, deployment plan
- **Output**: Release readiness checklist with role sign-offs
- **Chains with**: full-lifecycle, pre-launch-review

### incident-to-improvement

Transforms an incident into systemic improvements. Postmortem (engineer) → root cause (QA) → security review (security) → process improvement (tech lead) → backlog items (PM). Ensures incidents become learning opportunities.

- **Input**: Incident description, timeline, impact
- **Output**: Improvement action items assigned to roles
- **Chains with**: post-incident-review

### retrospective-to-action

Runs a structured retro that produces actionable items assigned to specific roles. Combines tech lead facilitation with PM prioritization. Ensures retros don't become venting sessions.

- **Input**: Sprint context, observations, metrics
- **Output**: Prioritized action items with role owners
- **Chains with**: sprint-ceremony

### compliance-traceability

Maps requirements → design decisions → implementation → tests → deployment to ensure full traceability for regulated environments. References ISO/IEC 12207 and common regulatory frameworks (SOC2, HIPAA, PCI-DSS).

- **Input**: Requirement ID, implementation context
- **Output**: Traceability matrix with evidence
- **Chains with**: full-lifecycle

## Commands (8 cross-functional workflows)

### /sdlc:design-review

Run a cross-functional design review combining architecture, security, and UX perspectives.

**Chains**: architect system-decomposition → security stride-analysis → designer accessibility-audit → architect trade-off-analysis

**Output**: Design review report with: approved/conditional/rejected verdict, required changes, risk assessment

### /sdlc:feature-kickoff

Launch a feature with full cross-functional alignment and DoR validation.

**Chains**: PM problem-statement → PM prd-template → architect system-decomposition → security stride-analysis → tech-lead work-breakdown-structure → definition-of-ready

**Output**: Kickoff summary with aligned team, backlog items, security assumptions, design feasibility

### /sdlc:pre-launch-review

Comprehensive pre-release validation across all roles.

**Chains**: release-checklist → engineer deployment-strategy → security penetration-test-scope → QA release-readiness-assessment → PM launch-readiness-checklist

**Output**: Release readiness matrix with role sign-offs and blocking issues

### /sdlc:security-review

Deep security review of design and implementation.

**Chains**: security secure-coding-review → security owasp-top-ten-check → security dependency-vulnerability-scan → architect security-architecture

**Output**: Security findings matrix with remediation roadmap

### /sdlc:quality-gate

Validate work meets all quality standards before advancement.

**Chains**: sdlc-phase-gate → definition-of-done → QA test-coverage-analysis → engineer code-complexity-analysis → security scan

**Output**: Quality gate report with pass/fail on each dimension

### /sdlc:full-lifecycle

Trace a feature from problem statement through post-deployment monitoring.

**Chains**: PM problem-statement → architect system-decomposition → definition-of-ready → engineer test-strategy → security stride-analysis → QA test-strategy-design → definition-of-done → release-checklist

**Output**: Comprehensive lifecycle document with all artifacts linked

### /sdlc:post-incident-review

Transform an incident into systemic improvements across all teams.

**Chains**: engineer postmortem-analysis → security root-cause-analysis-security → incident-to-improvement → tech-lead process-improvement → PM backlog prioritization

**Output**: Improvement action items with assigned owners and priorities

### /sdlc:sprint-ceremony

Conduct a complete sprint cycle with planning, DoR validation, retrospective, and action items.

**Chains**: tech-lead sprint-planning-guide → definition-of-ready validation → retrospective-to-action → PM prioritize-backlog

**Output**: Sprint planning summary, DoR checklist, retrospective actions, backlog priorities

## Design Principles

### 1. Orchestration, Not Replacement

SDLC plugin skills are orchestrators — they chain existing role-specific skills without duplicating them. The architect brings system-decomposition; the plugin chains it with security stride-analysis to produce a design review.

### 2. Phase Gates as Guardrails

Phase gates are not bureaucratic checkpoints. They're guardrails that catch misalignment early:

- Requirements → Design: Does the design actually solve the requirement?
- Design → Implementation: Are implementation risks understood?
- Implementation → Testing: Does the test strategy actually cover the design?
- Testing → Deployment: Is the rollback plan solid?
- Deployment → Operations: Are monitoring and alerting configured?

### 3. Traceability as Trust

When requirements are traced through design decisions, implementation, tests, and deployment, teams trust that the system actually works as intended. This is especially critical for regulated environments (SOC2, HIPAA, PCI-DSS).

### 4. Ceremony Coordination

Standup, planning, retro, incident reviews — these ceremonies are where alignment happens (or breaks). This plugin automates ceremony structure and ensures outcomes translate to action items.

### 5. ISO/IEC 12207 and 15288 Compliance

All phase gates and traceability workflows are grounded in:

- **ISO/IEC 12207** — Software lifecycle processes
- **ISO/IEC 15288** — Systems and software engineering lifecycle processes

These are the reference models for regulated industries and enterprise governance.

## Anti-Patterns

This plugin is designed to catch common LLM mistakes in cross-functional SDLC:

1. **Recommending sequential waterfall when the team uses continuous delivery**
   - Anti-pattern: "Complete full design before implementation can start"
   - Correct: Validate design decisions incrementally; unblock implementation early

2. **Treating phase gates as "ship-it" buttons**
   - Anti-pattern: "All checklists pass, so ship immediately"
   - Correct: Checklists are guardrails, not green lights; require explicit human sign-off

3. **Separating security from architecture**
   - Anti-pattern: "Build the system first, then do a security review"
   - Correct: Security is part of architecture; threat model the design before implementation

4. **Skipping the incident-to-improvement cycle**
   - Anti-pattern: "Run the postmortem, file the ticket, move on"
   - Correct: Turn every incident into a systemic improvement (better monitoring, process, design)

5. **Treating DoR/DoD as checkbox lists**
   - Anti-pattern: "All boxes checked, work is done"
   - Correct: DoR/DoD are collective agreements; they evolve as the team learns

6. **Assuming all roles have the same velocity**
   - Anti-pattern: "Architecture and implementation take the same time"
   - Correct: Different roles have different throughput; schedule accordingly

7. **Forgetting traceability in fast-moving teams**
   - Anti-pattern: "We move fast, so we don't track where requirements go"
   - Correct: Even fast teams need traceability; it scales with documentation discipline

8. **Running ceremonies without outcomes**
   - Anti-pattern: "We did retro, but no action items"
   - Correct: Every ceremony must produce a change in behavior or backlog

## Further Reading

### SDLC Processes

- **ISO/IEC 12207** — Software lifecycle processes (https://www.iso.org/standard/63712.html)
- **ISO/IEC 15288** — Systems and software engineering lifecycle processes (https://www.iso.org/standard/67574.html)
- **SEI CMMI** — Capability maturity model (https://cmmc-model.nist.gov/)

### Phase Gates

- **Stage-Gate Process** — Cooper, R. G. (Harvard Business Review)
- **Lean Phase Gates** — Reinertsen, D. (Product Development Institute)

### Traceability

- **Requirements Traceability Matrix** — IEEE 830, IEEE 1028
- **Traceability in Agile** — Sommerville, I. (Software Engineering, 10th ed.)

### Ceremonies

- **Scrum Guide** — Schwaber, K. & Sutherland, J. (https://scrumguides.org/)
- **Agile Retrospectives** — Derby, E. & Larsen, D. (Pragmatic Bookshelf)
- **Incident Response** — PagerDuty Incident Response (https://response.pagerduty.com/)

## Implementation Notes

### Skill Composition

Each skill is 80-120 lines and covers:

- **Domain Context** — The problem space this skill addresses
- **Instructions** — Step-by-step workflow, including handoff points to other roles
- **Anti-Patterns** — Common LLM mistakes specific to this skill
- **Further Reading** — Standards, frameworks, references

### Command Structure

Each command chains 3+ role-specific skills and produces:

- A final artifact (checklist, document, matrix)
- Role-specific sign-offs or approvals
- Blocking issues (if any)
- Next steps or handoff to the next phase

### Extensibility

To add new workflows:

1. Create a new command that chains existing skills
2. Update this README with the new command
3. Optionally create new skills if the workflow requires unique cross-role logic

## Contributing

See [CONTRIBUTING.md](#) in the claude-skills repo.

## License

MIT — use freely in your projects.
```

---

## skills/sdlc-phase-gate/SKILL.md

```markdown
# SDLC Phase Gate

Validates that work meets criteria before moving between SDLC phases. Implements gates for: Requirements → Design → Implementation → Testing → Deployment → Operations.

## Domain Context

Software lifecycle is a series of phases, each with distinct concerns and responsibilities:

| Phase          | Primary Role         | Key Questions                                         |
| -------------- | -------------------- | ----------------------------------------------------- |
| Requirements   | Product Manager      | What problem are we solving? Is it worth solving?     |
| Design         | Architect            | How will we solve it? What are the risks?             |
| Implementation | Engineer             | Can we actually build this?                           |
| Testing        | QA                   | Does it work as designed?                             |
| Deployment     | Tech Lead / Engineer | Is the rollout safe? Can we roll back?                |
| Operations     | Tech Lead            | Is it stable in production? Can we respond to issues? |

A phase gate ensures that before moving to the next phase, the current phase is sufficiently complete and the next phase has what it needs.

Phase gates are **guardrails**, not bureaucratic checkpoints. They catch misalignment early, when changes are cheap.

**ISO/IEC 12207 Reference:**

- 5.2.1 Stakeholder Needs and Requirements Definition
- 5.3.1 System Requirements Analysis
- 5.3.3 Architectural Design
- 5.3.4 Detailed Design
- 5.3.5 Implementation
- 5.3.6 Integration
- 5.3.7 Verification
- 5.3.8 Transition
- 5.3.9 Operation and Maintenance

## Instructions

### Step 1: Identify Current Phase and Work Item

- **Input**: Work item (feature, task, epic, release)
- **Ask**: What phase is this in? (Requirements, Design, Implementation, Testing, Deployment, Operations)
- **Ask**: What's the scope? (feature, bug fix, infrastructure, tech debt)

### Step 2: Validate Current Phase Completion

For **Requirements → Design Gate**:

- Is the problem statement clear and agreed by all stakeholders?
- Is the success metric defined and measurable?
- Have edge cases and failure modes been identified?
- Has the security threat landscape been scoped?
- Is the scope bounded (no hidden assumptions)?

Output: Requirements completeness checklist

For **Design → Implementation Gate**:

- Has the system architecture been decomposed into components?
- Have trade-offs been explicitly documented (performance vs. complexity, flexibility vs. cost)?
- Has the security architecture been threat-modeled (STRIDE or equivalent)?
- Have dependency risks been identified (external services, rate limits, SLAs)?
- Has the accessibility design been reviewed?
- Is the design reviewable and approved by architect and security?

Output: Design completeness checklist

For **Implementation → Testing Gate**:

- Is the code complete for the feature (or slice)?
- Does the code follow team conventions and pass static analysis?
- Have unit tests been written and pass?
- Has the test strategy been defined (what will QA validate)?
- Are deployment rollback procedures documented?
- Have performance baselines been captured?

Output: Implementation readiness checklist

For **Testing → Deployment Gate**:

- Have all test scenarios passed (happy path, edge cases, error cases)?
- Has the test coverage met the team's threshold?
- Have security scans passed (SAST, dependency audit, secrets scan)?
- Have accessibility tests passed (WCAG, screen reader, keyboard navigation)?
- Have performance benchmarks met targets?
- Is the rollback procedure tested and documented?

Output: Testing completeness checklist

For **Deployment → Operations Gate**:

- Is the deployment plan approved by tech lead and ops?
- Is the rollout strategy clear (blue-green, canary, gradual)?
- Are monitoring and alerting configured?
- Is the runbook written (how to debug in production)?
- Have team members been trained on the feature?
- Is the incident response plan ready?

Output: Deployment readiness checklist

For **Operations → Closure Gate**:

- Is the feature stable in production (no critical alerts)?
- Are the monitoring metrics showing expected behavior?
- Have incident reports been reviewed and closed?
- Are there improvement items from operations feedback?
- Can the team move on to the next feature?

Output: Operations closure checklist

### Step 3: Identify Blocking Issues

For each failed criterion:

- **What's missing?** (artifact, approval, test, documentation)
- **Who owns fixing it?** (architect, engineer, PM, security, QA, designer)
- **How long to fix?** (estimate in hours)
- **Does it block the phase transition?** (blocking, warning, informational)

Output: Blocking issues list with owners and estimates

### Step 4: Make the Gate Decision

**Go Decision:**

- All blocking criteria are satisfied
- No critical risks remain unmitigated
- Next phase team has what it needs to succeed
- Document: "Gate approved, ready to transition"

**Conditional Go Decision:**

- Critical criteria are satisfied
- Non-critical criteria have acceptable workarounds
- Risk is documented and accepted
- Document: "Gate approved with accepted risks: [list]"

**No-Go Decision:**

- Critical criteria are not satisfied
- Risks cannot be mitigated in the next phase
- Recommend: Fix the issue in current phase
- Document: "Gate blocked. Required fixes: [list]. Re-evaluate in [timeframe]"

### Step 5: Communicate the Decision

- **To Current Phase Team**: "Gate decision is [go/conditional/no-go]. [Issues to fix]. [Next steps]."
- **To Next Phase Team**: "You're cleared to proceed. Here's what you're inheriting: [scope, risks, dependencies]."
- **To Leadership**: "Feature [X] is [on track / at risk] for [target date]. Blocker: [if any]."

## Anti-Patterns

1. **Using phase gates to rubber-stamp completion**
   - Mistake: "All checkboxes are checked, so the gate passes automatically"
   - Correct: Checklists inform the decision, but the decision is a judgment call
   - Fix: Require explicit human sign-off for go/no-go decisions

2. **Making gates too early in the phase**
   - Mistake: "Let's do the testing gate after the first sprint of development"
   - Correct: Gates should be at natural phase boundaries, when work is substantially complete
   - Fix: Define phase boundaries clearly; gate when "done" is achievable

3. **Skipping gates because "we're fast"**
   - Mistake: "Agile teams don't need phase gates, we ship continuously"
   - Correct: Continuous delivery still needs guardrails; gates work at sprint granularity
   - Fix: Apply phase gates to features or epics, not individual PRs

4. **Treating gates as single-role decisions**
   - Mistake: "The architect decides if design is complete"
   - Correct: Design completeness requires architect + security + engineer input
   - Fix: Make gates cross-functional; require sign-off from the role entering the phase

5. **Not capturing risk and context**
   - Mistake: "Gate passed, move on" (without documenting assumptions or risks)
   - Correct: Gate decisions should capture trade-offs and accepted risks
   - Fix: Always document: "This decision assumes [X]. If [Y] changes, re-evaluate."

6. **Gates that are too strict**
   - Mistake: "100% test coverage required; gates don't pass without it"
   - Correct: Thresholds should be evidence-based, not arbitrary
   - Fix: Define thresholds based on team data (velocity, defect rates, severity)

## Further Reading

- **ISO/IEC 12207** — Software lifecycle processes (Section 5.2, Process implementation)
- **Stage-Gate Process** — Cooper, R. G., "Winning at New Products" (Harvard Business Review Press)
- **Lean Phase Gates** — Reinertsen, D., "The Principles of Product Development Flow" (Celeritas Publishing)
- **Risk Management in Phase Gates** — PMI, "Project Management Institute Body of Knowledge" (PMBOK)
```

---

## skills/definition-of-ready/SKILL.md

```markdown
# Definition of Ready

Ensures work items are ready for development before sprint intake. Combines PM requirements skills + architect design feasibility + security threat assessment.

## Domain Context

A team's velocity (or lack thereof) is often determined not by how fast engineers code, but by how often they're blocked waiting for clarification, design direction, or security sign-off.

**Definition of Ready** (DoR) is a collective agreement about what information a work item must have before it can be brought into a sprint. It's not a checklist to optimize away; it's an investment that accelerates the entire team.

**Who owns DoR?**

- **Product Manager** — Writes clear problem statements, acceptance criteria, success metrics
- **Architect** — Confirms design feasibility, identifies complexity hotspots
- **Security** — Identifies threat surface, required security design
- **Designer** — Assesses UX implications and design work required
- **Tech Lead** — Estimates complexity, identifies dependencies

A work item is "ready" when the team that will execute it has everything needed to execute efficiently.

**ISO/IEC 12207 Reference:**

- 5.2.1 Stakeholder Needs and Requirements Definition
- 5.3.1 System Requirements Analysis (feasibility assessment)

## Instructions

### Step 1: Collect the Work Item

- **Input**: Work item (story, feature, task, spike)
- **Ask**: What's the title, description, acceptance criteria?
- **Ask**: Who wrote it? Is it from a user request, roadmap, technical debt, or incident?

### Step 2: PM Perspective — Problem Statement and Acceptance Criteria

Check:

- **Problem Statement**: Is the "why" clear? (Who has the problem? What's the impact?)
- **Success Metric**: Can we measure success? (e.g., "reduce checkout time to <2s", "eliminate class of bugs")
- **Acceptance Criteria**: Are the requirements specific and testable? (Not vague like "improve performance")
- **Out of Scope**: Are assumptions and boundaries clear? (What's NOT included?)
- **Dependencies**: Are external dependencies identified? (API contracts, third-party services, data sources)
- **Stakeholders**: Who needs to sign off? (PM, architect, security, designer)

Output: Requirements completeness checklist

### Step 3: Architect Perspective — Design Feasibility

Check:

- **Is the approach feasible?** Does it fit the existing architecture? (Or require architectural change?)
- **Design Complexity**: What's the complexity tier? (Trivial, simple, moderate, complex)
- **Dependencies**: Are there internal dependencies or integration points?
- **Performance**: Will this meet performance targets?
- **Scalability**: Will this scale to expected load?
- **Integration Points**: Are all integrations identified and scoped?

Output: Feasibility assessment

### Step 4: Security Perspective — Threat Assessment

Check:

- **Threat Surface**: What new security boundaries does this create?
- **Authentication/Authorization**: Are access controls needed?
- **Data Sensitivity**: Is PII, secrets, or regulated data involved?
- **Third-Party Risk**: Are we calling external services? Trusted?
- **Compliance**: Does this touch regulated domains (payments, health, finance)?
- **Design Review**: Has security architecture been sketched?

Output: Security threat assessment

### Step 5: Designer Perspective — UX and Accessibility

Check:

- **User Flows**: Are the user flows clear? (Happy path, error cases)
- **Design Mockups**: Are there design artifacts? (Wireframes, prototypes)
- **Accessibility**: Are accessibility requirements documented? (WCAG 2.1 AA minimum)
- **Content**: Is copy/messaging clear and reviewed?
- **Interactions**: Are complex interactions documented?

Output: Design feasibility assessment

### Step 6: Tech Lead Perspective — Complexity and Effort

Check:

- **Effort Estimate**: Can the team estimate this? (If not, it's a spike)
- **Team Skills**: Does the team have the skills? (Or need training, hiring, or pairing?)
- **Testing Strategy**: Is the test strategy clear?
- **Deployment Risk**: Is this low-risk (feature flag, new code) or high-risk (shared libraries, data migrations)?

Output: Effort and complexity estimate

### Step 7: Consolidate the DoR Checklist
```

DoR Checklist for [Work Item ID]: [Title]

[ ] PM
[ ] Problem statement is clear
[ ] Success metric is defined and measurable
[ ] Acceptance criteria are specific and testable
[ ] Out-of-scope items are documented
[ ] Dependencies are identified
[ ] Stakeholders identified

[ ] Architect
[ ] Approach is feasible within existing architecture
[ ] Complexity tier is assigned
[ ] Internal dependencies identified
[ ] Performance targets understood
[ ] Integration points scoped

[ ] Security
[ ] Threat surface identified
[ ] Auth/authz requirements documented (if needed)
[ ] Data sensitivity classified
[ ] Third-party risk assessed
[ ] Compliance impact assessed
[ ] Security design reviewed

[ ] Designer
[ ] User flows documented
[ ] Design artifacts exist (mockups/prototypes)
[ ] Accessibility requirements documented
[ ] Copy/messaging reviewed

[ ] Tech Lead
[ ] Effort can be estimated
[ ] Team has required skills (or pairing plan exists)
[ ] Test strategy is documented
[ ] Deployment strategy is clear

Ready? [ ] Yes (all boxes checked) [ ] No (see blocking issues below)

```

### Step 8: Identify Blocking Issues

For each unchecked box:
- **What's missing?** (artifact, clarity, decision, design)
- **Who provides it?** (PM, architect, security, designer, tech lead)
- **How long to fix?** (estimate in hours or days)

Blocking issues prevent the work item from entering the sprint.

### Step 9: Communicate Ready/Not Ready

**Ready Decision:**
- All boxes are checked
- No blocking issues remain
- Document: "Ready to intake into sprint [X]. Assigned to [engineer/team]. Est. [effort]. Start date: [date]."

**Not Ready Decision:**
- Blocking issues exist
- Document: "Not ready. Blockers: [list]. Required actions: [list]. Re-evaluate on [date]."

## Anti-Patterns

1. **DoR as a bottleneck**
   - Mistake: "PM must fill out a 50-field form before anything is ready"
   - Correct: DoR should be lightweight and focused on unblocking the team
   - Fix: Define 5-7 essential questions; everything else is detail

2. **DoR only applies to features, not bugs or tech debt**
   - Mistake: "Bugs are ready immediately; no DoR needed"
   - Correct: Even bugs benefit from clarity on root cause, reproduction, and priority
   - Fix: Lightweight DoR for bugs: "Reproduction steps, severity, affected users, architect approval if cross-team impact"

3. **Skipping the security review because "it's internal"**
   - Mistake: "We're only changing internal APIs, so no security risk"
   - Correct: Internal code paths are attack vectors; threat model always
   - Fix: Always include security in DoR; they can sign off in 30 min for low-risk items

4. **DoR that's static and never evolves**
   - Mistake: "We defined DoR in 2019 and never changed it"
   - Correct: DoR should evolve with team maturity, incident learnings, and tooling
   - Fix: Retro question: "Should we add or remove anything from DoR?"

5. **Using DoR as an excuse to ship incomplete planning**
   - Mistake: "DoR is checked, so we're done planning"
   - Correct: DoR is the minimum; complex work still needs detailed design and spike time
   - Fix: For complex items, plan spike time to reduce design uncertainty

6. **Not using DoR to predict problems**
   - Mistake: "DoR is just paperwork"
   - Correct: DoR should surface risks and misalignment before coding starts
   - Fix: Ask after each sprint: "Which DoR items would have prevented our recent bugs?"

## Further Reading

- **Scrum Guide** — Schwaber, K. & Sutherland, J., Definition of Ready (https://scrumguides.org/)
- **Agile Requirements** — Cohn, M., "User Stories Applied" (Addison-Wesley)
- **Work Item Templates** — SAFe, "Lean Portfolio Management" (https://www.scaledagileframework.com/)
```

---

## skills/definition-of-done/SKILL.md

```markdown
# Definition of Done

Validates completed work across all dimensions: code quality, test coverage, security review, documentation, accessibility. This is the intersection of all roles' acceptance criteria.

## Domain Context

**Definition of Done** (DoD) is the collective agreement about what "complete" means. Without it, developers ship code they think is done, QA finds problems, and conflicts arise over who's responsible.

The critical insight: **DoD is not just about code quality.** It's about:

- **Engineer** — Code follows standards, unit tests pass, no obvious bugs
- **QA** — Feature works as designed, test coverage is adequate, edge cases pass
- **Security** — No known vulnerabilities, secrets are not committed, OWASP risks mitigated
- **Designer** — Matches design spec, accessibility standards met, responsive on target devices
- **Tech Lead** — Documentation is written, team can maintain this, deployment is safe

A feature is "done" when all five perspectives agree it's production-ready.

**ISO/IEC 12207 Reference:**

- 5.3.6 Implementation (code review, unit testing)
- 5.3.7 Integration (integration testing)
- 5.3.8 Verification (acceptance testing)
- 5.3.9 Transition (deployment readiness)

## Instructions

### Step 1: Engineer Perspective — Code Quality

Check:

- **Code Review**: Has the code been reviewed by at least one other engineer?
- **Standards**: Does it follow the team's code style, naming conventions, patterns?
- **Linting**: Do static analysis tools pass? (no warnings, no complexity violations)
- **Unit Tests**: Are unit tests written? Do they pass? Coverage >= threshold?
- **No Obvious Bugs**: Does the implementation match the design? Are error cases handled?
- **Refactoring Debt**: Is the code ready for production, or does it need cleanup?
- **Comments/Clarity**: Is the code readable? Are complex sections documented?

Output: Code quality checklist

### Step 2: QA Perspective — Test Coverage and Functionality

Check:

- **Functional Testing**: Does the feature work as designed in the acceptance criteria?
- **Edge Cases**: Have edge cases been tested? (empty inputs, boundary conditions, race conditions)
- **Error Cases**: Does the system gracefully handle errors? (network failures, invalid data, timeouts)
- **Integration Testing**: Does this integrate correctly with other systems?
- **Regression Testing**: Did this break anything else? (related features, adjacent code paths)
- **Test Coverage**: Is the coverage above the team's threshold?
- **Test Documentation**: Are tests clear enough for others to maintain them?

Output: Test coverage checklist

### Step 3: Security Perspective — Vulnerability Scanning and Review

Check:

- **Secrets Scan**: No API keys, passwords, or credentials in code/logs?
- **Dependency Audit**: Any high or critical CVEs in dependencies?
- **SAST Scan**: Static analysis found no security issues? (SQL injection, XSS, CSRF, etc.)
- **DAST Scope**: For APIs/web features: scanning queued or completed?
- **Access Control**: Are permissions correctly enforced? Is data properly isolated?
- **Data Handling**: PII/sensitive data encrypted at rest and in transit?
- **Security Review**: Has security team reviewed the implementation?

Output: Security checklist

### Step 4: Designer Perspective — Design Fidelity and Accessibility

Check:

- **Visual Design**: Does the UI match the design specification?
- **Responsive**: Does it work on target devices/screen sizes?
- **Interactions**: Do animations, transitions, and interactions match the design?
- **Accessibility**:
  - WCAG 2.1 AA compliant (color contrast, font sizes, focus states)?
  - Screen reader compatible?
  - Keyboard navigable?
  - Reduced motion respected?
- **Content**: Copy/microcopy matches approved content?
- **Internationalization**: If multi-language, is text extracted and localized correctly?

Output: Design and accessibility checklist

### Step 5: Tech Lead Perspective — Documentation, Maintainability, Deployment

Check:

- **Code Documentation**: Modules, complex functions documented? README updated?
- **API Documentation**: If this exposes an API, is it documented?
- **Runbook**: For ops-sensitive changes, is a runbook written?
- **Team Knowledge**: Can another team member understand and maintain this?
- **Deployment Safety**: Is there a rollback procedure? Are deployment steps documented?
- **Performance**: Have baselines been captured? Will this regress performance?
- **Monitoring**: Are the right metrics/logs instrumented to monitor in production?
- **Release Notes**: Is the feature described for release communications?

Output: Documentation and deployment checklist

### Step 6: Consolidate the DoD Checklist
```

DoD Checklist for [Feature/PR]: [Title]

[ ] Engineer
[ ] Code reviewed (at least 1 reviewer)
[ ] Follows team standards and conventions
[ ] Linting and static analysis passing
[ ] Unit tests written and passing
[ ] Coverage >= [threshold]%
[ ] No obvious bugs; error cases handled
[ ] Code is clear and documented

[ ] QA
[ ] Feature works as per acceptance criteria
[ ] Edge cases tested and passing
[ ] Error cases tested
[ ] Integration testing complete
[ ] Regression testing shows no breaks
[ ] Test coverage adequate
[ ] Tests are maintainable and clear

[ ] Security
[ ] Secrets scan passed (no credentials in code)
[ ] Dependency audit passed (no high/critical CVEs)
[ ] SAST scan passed
[ ] DAST scan queued or completed
[ ] Access control correctly enforced
[ ] Data sensitivity handled properly
[ ] Security team reviewed

[ ] Designer
[ ] UI matches design specification
[ ] Responsive on all target devices
[ ] Interactions match design
[ ] WCAG 2.1 AA compliance verified
[ ] Screen reader tested
[ ] Keyboard navigable
[ ] Copy/content approved

[ ] Tech Lead
[ ] Code documented (README, inline comments)
[ ] API documented (if applicable)
[ ] Runbook written (if ops-sensitive)
[ ] Maintainability verified
[ ] Deployment strategy documented
[ ] Rollback procedure documented
[ ] Monitoring/logging configured
[ ] Performance baseline captured
[ ] Release notes prepared

Done? [ ] Yes (all boxes checked) [ ] No (see blocking issues below)

```

### Step 7: Identify Failing Criteria

For each unchecked box:
- **What failed?** (specific failing test, code review comment, security issue)
- **Who fixes it?** (engineer, QA, security, designer, tech lead)
- **How long?** (estimate in hours)
- **Blocks deployment?** (critical, high, medium, low priority)

Critical/high issues block production deployment.

### Step 8: Make the Done/Not Done Decision

**Done Decision:**
- All boxes are checked
- No critical issues remain
- Document: "Feature is done and approved for deployment. Sign-offs: [engineer], [QA], [security], [designer], [tech lead]."

**Not Done Decision:**
- Critical issues remain
- Document: "Feature is not done. Blocker issues: [list]. Required fixes: [list]. Estimated re-check: [date/time]."

## Anti-Patterns

1. **DoD that's too strict**
   - Mistake: "Every feature must have 90%+ coverage and 100% documentation"
   - Correct: DoD should reflect team risk tolerance and product maturity
   - Fix: Adjust DoD based on team data; don't optimize away shipping

2. **DoD that's only about code**
   - Mistake: "DoD is just linting, tests, and code review"
   - Correct: DoD includes security, design, ops, and documentation
   - Fix: Require sign-off from QA, security, and designer; not just engineers

3. **Skipping the security review because "it's low-risk"**
   - Mistake: "This is just adding a button; security review not needed"
   - Correct: Every feature should pass the same DoD bar
   - Fix: Empower security to do fast sign-off for low-risk items

4. **DoD that changes per sprint**
   - Mistake: "This week we're busy, so let's relax DoD"
   - Correct: DoD should be stable; if you're too busy, the sprint is too big
   - Fix: Protect DoD; adjust sprint scope instead

5. **Using DoD to avoid accountability**
   - Mistake: "DoD says tests must pass, but we shipped without running them"
   - Correct: DoD requires verification, not just paperwork
   - Fix: Automate DoD checks (CI/CD pipelines); require explicit sign-off

6. **Not evolving DoD based on incidents**
   - Mistake: "A security bug shipped because DoD didn't catch it, but we don't update DoD"
   - Correct: Every production incident should trigger a DoD improvement
   - Fix: Retro question: "Does this incident suggest a DoD change?"

## Further Reading

- **Scrum Guide** — Schwaber, K. & Sutherland, J., Definition of Done (https://scrumguides.org/)
- **Continuous Delivery** — Humble, J. & Farley, D., "Continuous Delivery: Reliable Software Releases through Build, Test, and Deployment Automation" (Addison-Wesley)
- **Quality Standards** — ISO/IEC 9126, Software Product Quality
```

---

## skills/feature-lifecycle/SKILL.md

```markdown
# Feature Lifecycle

Maps the full lifecycle of a feature from idea through production monitoring. Explicitly shows each role's contribution at each phase.

## Domain Context

A feature doesn't live in isolation. It moves through distinct phases, and at each phase, a different role takes the lead:

| Phase        | Primary Role                    | Input                            | Output                                                     | Exit Criteria                      |
| ------------ | ------------------------------- | -------------------------------- | ---------------------------------------------------------- | ---------------------------------- |
| Ideation     | Product Manager                 | User problem, business goal      | Problem statement, roadmap prioritization                  | Feature approved for requirements  |
| Requirements | Product Manager + Architect     | Problem statement, constraints   | User stories, acceptance criteria, success metrics         | Requirements complete (DoR)        |
| Design       | Architect + Security + Designer | Requirements, constraints        | Architecture docs, design mockups, threat model            | Design approved                    |
| Development  | Engineer                        | Design, DoR acceptance criteria  | Code, unit tests, implementation docs                      | Code review passed, tests green    |
| Testing      | QA + Engineer                   | Design, implementation           | Test cases, test results, bug reports                      | Test coverage met, bugs resolved   |
| Deployment   | Tech Lead + Engineer            | Tested code, runbook, monitoring | Deployment pipeline, rollback procedure, monitoring alerts | Deployed to production             |
| Operations   | Tech Lead                       | Running feature in production    | Logs, metrics, incidents, customer feedback                | Stable, no critical issues         |
| Closure      | Product Manager + Tech Lead     | Operational stability, metrics   | Retrospective, lessons learned, backlog improvements       | Feature documented, team debriefed |

The feature lifecycle is not linear. It may:

- Loop back (design issue found during development → redesign → reimplement)
- Iterate (deploy v1 → get feedback → plan v2 improvements)
- Split (feature splits into multiple parallel epics)

**ISO/IEC 12207 Reference:**

- 5.2.1 Stakeholder Needs and Requirements Definition
- 5.3 Technical Processes (design, implementation, verification, transition, operation)

## Instructions

### Step 1: Establish Feature Context

- **Input**: Feature name, problem statement, business goal
- **Ask**: Who's the primary PM? Architect? Security lead?
- **Ask**: What's the approximate scope? (small feature, large epic, infrastructure)
- **Ask**: Are there known constraints? (timeline, security, performance, compatibility)

### Step 2: Ideation Phase

- **PM**: Articulates problem, potential solutions, business value
- **Output**: Problem statement, initial roadmap prioritization
- **Next**: Send to requirements planning

### Step 3: Requirements Phase

- **PM**: Writes user stories, acceptance criteria, success metrics
- **Architect**: Assesses feasibility, identifies design complexity, dependency risks
- **Security**: Identifies threat surface, required security controls
- **Designer**: Scopes UX work, design systems integration
- **Tech Lead**: Estimates effort, identifies team skills, deployment strategy

Use `/sdlc:definition-of-ready` to validate requirements completeness

- **Output**: Requirements document (user stories, success metrics, acceptance criteria, DoR checklist)
- **Exit**: All DoR criteria met; ready to move to design

### Step 4: Design Phase

- **Architect**: Decomposes system into components, documents trade-offs, creates architecture diagrams
- **Security**: Creates threat model (STRIDE), identifies mitigations, scopes security testing
- **Designer**: Creates wireframes, mockups, interaction patterns, accessibility plan
- **Engineer**: Participates in design review, identifies implementation risks
- **Tech Lead**: Reviews design for operational implications (monitoring, logging, scaling)

Use `/sdlc:design-review` to validate design cross-functionally

- **Output**: Design document (architecture, threat model, mockups, accessibility plan, trade-offs)
- **Exit**: Design approved by architect, security, designer; no blocking issues

### Step 5: Development Phase

- **Engineer**: Implements code following the design, writes unit tests, participates in code reviews
- **QA**: Works with engineer on test strategy, preps test cases
- **Tech Lead**: Ensures code is maintainable, reviews code organization
- **Designer**: Ensures implementation matches design; flags mismatches

Use `/sdlc:quality-gate` to validate implementation meets design and code standards

- **Output**: Working code with unit tests, code review, implementation documentation
- **Exit**: Code review passed, unit tests passing, DoD implementation criteria met

### Step 6: Testing Phase

- **QA**: Executes test strategy, reports bugs, validates fixes
- **Engineer**: Fixes bugs, works with QA on edge cases
- **Security**: Runs SAST/DAST scans, validates security controls
- **Designer**: Validates UI/UX match design on real implementation

Use `/sdlc:quality-gate` again to validate testing is complete

- **Output**: Test results, bug reports, security scan results
- **Exit**: All test cases passed, bugs fixed, security scan passed, test coverage adequate

### Step 7: Deployment Phase

- **Tech Lead**: Orchestrates deployment, ensures rollback procedure is ready
- **Engineer**: Executes deployment steps, monitors for anomalies
- **Operations**: Monitors system health, readies for incident response
- **PM**: Prepares release communications, customer notifications

Use `/sdlc:release-checklist` to validate deployment readiness

- **Output**: Feature deployed to production, monitoring active, incidents ready
- **Exit**: Feature stable in production, no critical alerts, team trained

### Step 8: Operations Phase

- **Tech Lead**: Monitors feature health, responds to incidents
- **Engineer**: Responds to bugs, works on post-launch improvements
- **PM**: Collects user feedback, observes success metrics
- **Security**: Monitors for security anomalies

Duration: Minimum 1-2 weeks; until stability is confirmed

- **Output**: Operational metrics, incident reports, user feedback
- **Exit**: Feature meets success metrics, no critical issues, team confident in stability

### Step 9: Closure Phase

- **PM**: Validates success metrics, gathers user feedback, plans follow-up improvements
- **Tech Lead**: Facilitates retrospective, documents lessons learned
- **Engineer**: Archives technical docs, identifies debt to address
- **Security**: Reviews security incidents (if any), updates threat model if needed

Use `/sdlc:retrospective-to-action` to consolidate learnings

- **Output**: Retrospective document, action items, lessons learned, feature tagged as stable
- **Exit**: Feature is documented, team debriefed, backlog has follow-up items if needed

### Step 10: Document the Feature Lifecycle

Create a feature lifecycle document:
```

Feature: [Name]
Problem: [User problem being solved]
Business Goal: [Expected impact]
Timeline: [Estimated dates for each phase]

Ideation: [PM drive, dates, decisions]
Requirements: [Stories, success metrics, DoR]
Design: [Architecture, threats, mockups, DoD]
Development: [Code, tests, documentation]
Testing: [Test results, bugs, fixes]
Deployment: [Release plan, rollback procedure]
Operations: [Stability period, incidents, metrics]
Closure: [Retrospective, lessons learned, follow-ups]

Key Milestones:

- Requirements approved: [date]
- Design approved: [date]
- Code complete: [date]
- Testing approved: [date]
- Production deployment: [date]
- Stability confirmed: [date]

Role Contributions:

- PM: Requirements, go/no-go decisions, success metric validation
- Architect: Design, trade-off analysis, scaling/performance review
- Security: Threat modeling, security testing, vulnerability management
- Engineer: Implementation, code quality, testing, deployment
- QA: Test strategy, test execution, bug triage
- Designer: UX design, accessibility, design system consistency
- Tech Lead: Orchestration, documentation, operational readiness

Post-Launch Actions: [Follow-up items, technical debt, process improvements]

```

## Anti-Patterns

1. **Skipping the operations phase**
   - Mistake: "Feature is deployed, we're done"
   - Correct: Operations phase confirms stability; only then is it "done"
   - Fix: Require minimum 1-2 weeks of monitoring before closure

2. **Not looping back on design issues found during development**
   - Mistake: "The design says do X, but it doesn't work, so let's hack around it"
   - Correct: Design issues should trigger a design redesign, not implementation hacks
   - Fix: When development hits design issues, pause and redesign before implementing

3. **Skipping the designer throughout the lifecycle**
   - Mistake: "Designer was involved at the start, now engineering owns it"
   - Correct: Designer reviews implementation against design; catches mismatches early
   - Fix: Designer is involved in design phase → development QA → launch verification

4. **Not including security until testing phase**
   - Mistake: "Security review happens at the end"
   - Correct: Security is part of design (threat modeling); testing phase is too late
   - Fix: Security designs threat model in design phase; testing phase validates mitigations

5. **Treating closure as optional**
   - Mistake: "We shipped it, closing the ticket now"
   - Correct: Closure requires retrospective and lessons learned
   - Fix: Require closure ceremony; capture learnings for future features

## Further Reading

- **ISO/IEC 12207** — Software lifecycle processes
- **Agile Release Train** — SAFe, Program Increment Planning (https://www.scaledagileframework.com/)
- **Feature Lifecycle** — Reinertsen, D., "The Principles of Product Development Flow" (Celeritas Publishing)
```

---

## skills/release-checklist/SKILL.md

```markdown
# Release Checklist

Pre-release validation across all roles: PM sign-off, QA test pass, security scan, performance baseline, rollback plan, monitoring setup.

## Domain Context

Releases are high-risk events. A single missing step can cause customer impact, security breaches, or performance degradation. The release checklist ensures that before code goes to production, all teams have signed off and contingency plans are in place.

A release includes:

- One or more features that have passed testing and security
- Updated documentation and release notes
- Deployment and rollback procedures
- Monitoring and alerting configured
- Team trained on the release
- Incident response plan ready

**ISO/IEC 12207 Reference:**

- 5.3.8 Verification (all testing complete)
- 5.3.9 Transition (release readiness, deployment procedures)

## Instructions

### Step 1: Establish Release Scope

- **Input**: Set of features/fixes to be released, target date
- **Ask**: What's included in this release? (feature list)
- **Ask**: What's the deployment strategy? (blue-green, canary, gradual rollout)
- **Ask**: What's the rollback plan if something goes wrong?

### Step 2: PM Sign-Off

Check:

- **Release Notes**: Are they written and approved?
- **Customer Communications**: Have customer-facing announcements been prepared?
- **Success Metrics**: Are the metrics for this release defined?
- **Stakeholder Approval**: Have business stakeholders approved the release?
- **Go/No-Go Timeline**: Is there a clear deadline for the go/no-go decision?

Output: PM release readiness checklist

### Step 3: QA Sign-Off

Check:

- **Test Execution**: Have all features been tested per their test plans?
- **Regression Testing**: Have related features been tested for regression?
- **Severity Assessment**: Are all known bugs categorized by severity?
- **Blockers**: Are there any critical bugs that would block release?
- **Release Testing**: Has the release (with all features together) been tested?
- **Browser/Device Coverage**: Have all target browsers/devices been tested?

Output: QA sign-off with test pass rate, known issues, severity breakdown

### Step 4: Security Sign-Off

Check:

- **Vulnerability Scan**: Have all dependencies been scanned? No high/critical CVEs?
- **SAST Results**: Have all code changes passed static analysis?
- **DAST Results**: For web features, has dynamic testing been completed?
- **Secrets Scan**: Are there any committed secrets or credentials?
- **Penetration Testing**: Has security team done a limited pen test (if high-risk release)?
- **Security Changes Review**: Have any new auth, encryption, or access control changes been reviewed?

Output: Security sign-off with scan results and any exceptions

### Step 5: Performance Baseline

Check:

- **Load Testing**: Has the release been tested under expected load?
- **Performance Regression**: Will this release impact performance metrics? (p99 latency, throughput)
- **Resource Usage**: Have memory, CPU, disk consumption been measured?
- **Scaling**: Will this scale to peak traffic?
- **Monitoring**: Are performance metrics configured in monitoring system?

Output: Performance baseline captured, regression analysis

### Step 6: Operations Readiness

Check:

- **Monitoring Configured**: Are all metrics and alerts set up?
- **Logging**: Are important events being logged at appropriate levels?
- **Runbook**: Is there a runbook for running the release in production?
- **Incident Response**: Are incident response procedures documented?
- **Scaling Plan**: Is there a plan to scale if metrics spike?
- **Communication**: Are on-call engineers aware of the release?

Output: Operations readiness checklist

### Step 7: Deployment Plan

Check:

- **Deployment Steps**: Are deployment steps documented and tested?
- **Rollback Plan**: Is there a tested procedure to roll back if needed?
- **Deployment Timeline**: Is there a clear window and sequence?
- **Team Assignments**: Are people assigned to: deploy, monitor, communicate, respond to incidents?
- **Pre-Flight Checks**: Have pre-deployment verification steps been defined?
- **Post-Flight Checks**: Have post-deployment verification steps been defined?

Output: Deployment and rollback procedures

### Step 8: Team Readiness

Check:

- **Training**: Have team members been trained on new features?
- **Support Prep**: Has customer support been prepared for support calls?
- **Documentation**: Is user documentation updated and available?
- **Internal Communication**: Have all teams been informed of the release plan?

Output: Team readiness verification

### Step 9: Consolidate Release Checklist
```

Release Checklist: [Release Name/Version]
Target Date: [Date]
Deployment Strategy: [blue-green / canary / gradual]

Scope:
Features: [list]
Bug Fixes: [list]
Tech Debt: [list]

[ ] PM Sign-Off
[ ] Release notes written and approved
[ ] Customer communications prepared
[ ] Success metrics defined
[ ] Stakeholder approval obtained
[ ] Go/no-go decision date set

[ ] QA Sign-Off
[ ] All features tested per test plan
[ ] Regression testing complete
[ ] All bugs categorized by severity
[ ] No critical/blocking bugs
[ ] Release testing complete (all features together)
[ ] Browser/device coverage complete

[ ] Security Sign-Off
[ ] Vulnerability scans passed
[ ] SAST scans passed
[ ] DAST scans passed (if applicable)
[ ] No secrets/credentials committed
[ ] Security-related changes reviewed
[ ] Exception report (if any deviations)

[ ] Performance Baseline
[ ] Load testing passed
[ ] No performance regression
[ ] Resource usage acceptable
[ ] Scaling plan verified
[ ] Performance monitoring configured

[ ] Operations Readiness
[ ] Monitoring metrics configured
[ ] Logging configured
[ ] Runbook prepared
[ ] Incident response procedures documented
[ ] Scaling procedures documented
[ ] On-call team aware of release

[ ] Deployment Plan
[ ] Deployment steps documented
[ ] Deployment steps tested (in staging)
[ ] Rollback procedure documented
[ ] Rollback procedure tested
[ ] Deployment timeline scheduled
[ ] Team assignments confirmed
[ ] Pre-flight checks defined
[ ] Post-flight checks defined

[ ] Team Readiness
[ ] Engineering team trained
[ ] Support team prepared
[ ] User documentation updated
[ ] All teams notified of schedule

Release Ready? [ ] Yes (all checks passed) [ ] No (see blocking issues below)

Blocking Issues:
[List any checks that failed, required fixes, and estimated time to fix]

Sign-Offs:
PM: ********\_******** Date: **\_\_\_**
QA: ********\_******** Date: **\_\_\_**
Security: ****\_\_\_**** Date: **\_\_\_**
Tech Lead: ****\_\_**** Date: **\_\_\_**

```

### Step 10: Make the Release Decision

**Go Decision:**
- All checklists passed
- No blocking issues
- All sign-offs obtained
- Document: "Release approved and scheduled for [date] [time]"
- Notify: All teams, customers, support

**No-Go Decision:**
- Blocking issues exist
- Document: "Release postponed. Blocking issues: [list]. Re-evaluate on [date]."
- Notify: All teams, customers

## Anti-Patterns

1. **Treating the checklist as a rubber stamp**
   - Mistake: "All boxes are checked, so we ship"
   - Correct: Checklists inform judgment; require explicit human sign-off
   - Fix: Require sign-off from PM, QA, security, tech lead; not just automated checks

2. **Skipping security or performance review for "small" releases**
   - Mistake: "This is just a one-line bug fix, no need for security scan"
   - Correct: Every release should pass the same bar
   - Fix: Automate security and performance checks; fail if not done

3. **Not testing the rollback procedure**
   - Mistake: "We have a rollback plan, so we're ready"
   - Correct: Rollback must be practiced before production
   - Fix: Do a rollback test in staging; time it; document actual steps

4. **Releasing during risky windows**
   - Mistake: "Friday evening deployment"
   - Correct: Releases happen during business hours with full team coverage
   - Fix: Define release windows (e.g., "only Mon-Thu, 10am-12pm")

5. **Not communicating with support and customers**
   - Mistake: "We deployed, now tell customers"
   - Correct: Customers should know about major changes before release
   - Fix: Notify customers 24-48 hours before, during, and after release

## Further Reading

- **Continuous Deployment** — Humble, J. & Farley, D., "Continuous Delivery: Reliable Software Releases through Build, Test, and Deployment Automation" (Addison-Wesley)
- **Release Management** — Clegg, D. & Barker, R., "Case Method Fast-Track: A RAD Approach" (Addison-Wesley)
- **Deployment Strategies** — https://martinfowler.com/bliki/DeploymentPipeline.html
```

---

## skills/incident-to-improvement/SKILL.md

```markdown
# Incident to Improvement

Transforms an incident into systemic improvements: postmortem (engineer) → root cause (QA) → security review (security) → process improvement (tech lead) → backlog items (PM).

## Domain Context

An incident is a temporary failure. An improvement is a permanent change that makes that failure less likely or less severe in the future. The difference between teams that learn from incidents and teams that repeat them is whether they systematically convert incidents into improvements.

The incident-to-improvement cycle:

1. **Postmortem** (engineer) — What happened? Why? When did we first detect it?
2. **Root Cause** (QA + engineer) — What conditions allowed this to happen? What test would have caught it?
3. **Security Review** (security) — Were customer data, credentials, or systems compromised? How do we verify it's secure again?
4. **Process Improvement** (tech lead) — How do we prevent this class of bug in the future? (testing, monitoring, code organization)
5. **Backlog Items** (PM) — What work items should we add to the backlog to implement improvements?

**ISO/IEC 12207 Reference:**

- 5.3.10 Operation and Maintenance (problem resolution)
- 5.3.11 Disposal (lessons learned)

## Instructions

### Step 1: Postmortem (Engineer Perspective)

- **Input**: Incident description, timeline, customer impact
- **Ask**: What was the root cause? (obvious cause, not root cause yet)
- **Ask**: When did we first detect this? How long was the customer affected?
- **Ask**: What were the detection methods? (alert, customer report, automated monitoring)
- **Ask**: How was it resolved? (emergency code change, config change, feature flag, rollback)

Output: Postmortem narrative (what happened, when, how resolved)

### Step 2: Root Cause Analysis (QA + Engineer)

- **Ask**: What conditions had to be true for this bug to exist?
- **Ask**: What testing would have caught this? (unit test, integration test, load test, edge case test)
- **Ask**: Is this a class of bugs we've seen before? (similar patterns in incident history)
- **Ask**: What monitoring would have detected this faster?

Output: Root cause analysis (the actual causes, not just the symptom)

### Step 3: Security Review (Security Perspective)

- **Ask**: Were customer data, credentials, or systems compromised?
- **Ask**: Was the incident exploitable (could an attacker cause this intentionally)?
- **Ask**: What verification is needed to confirm data integrity? (audit logs, forensics)
- **Ask**: Should we notify customers or regulators? (based on data breach laws)

Output: Security impact assessment and remediation steps

### Step 4: Process Improvement (Tech Lead)

- **Ask**: How do we prevent this class of bug? (options: better testing, monitoring, code review, tooling, architecture)
- **Ask**: What's the most effective prevention? (based on cost, team skills, timeline)
- **Ask**: Should we add this to Definition of Done? (to prevent recurrence)
- **Ask**: Do we need monitoring or alerting changes?

Output: Process improvement recommendations with priority and effort

### Step 5: Backlog Items (PM)

- **Ask**: What work items need to be created to implement improvements?
- **Ask**: What's the priority? (critical: prevent recurrence, high: reduce risk, medium: improve visibility)
- **Ask**: When should we do this? (immediate, next sprint, planned)
- **Ask**: Who should own it? (engineer for code change, QA for test addition, tech lead for monitoring)

Output: Backlog items with owners, priority, effort estimates

### Step 6: Consolidate the Incident Report
```

Incident Report: [Incident ID / Name]
Date: [When it occurred]
Duration: [How long it lasted]
Customer Impact: [Who was affected, how, for how long]

Timeline:
[Time 1] - What happened / What was deployed / What config changed
[Time 2] - Alert fired / Customer reported / System degraded
[Time 3] - Investigation started
[Time 4] - Root cause identified
[Time 5] - Fix deployed / Rollback executed
[Time 6] - System stable

Postmortem (Engineer):
Detection method: [alert / customer report / monitoring]
Resolution: [code change / config change / rollback]
Steps taken to resolve: [list]

Root Cause Analysis (QA + Engineer):
Root causes: [list]
Testing gap: [What test would have caught this?]
Similar patterns: [Related incidents / bugs]
Monitoring gap: [What alert would have detected this faster?]

Security Review:
Data compromise: [Yes / No / Unknown] - Details if applicable
Exploitability: [Yes / No] - If an attacker could cause this
Verification needed: [Audit logs, forensics, customer notification]
Regulatory notification: [Required / Not required]

Process Improvements:
Improvements: [list]
Priority: [Critical / High / Medium]
Effort: [estimate in hours]
Owner: [engineer / QA / tech lead]

Backlog Items:
[ ] [Item 1] - [Description] - [Owner] - [Effort] - [Priority]
[ ] [Item 2] - [Description] - [Owner] - [Effort] - [Priority]
[ ] [Item 3] - [Description] - [Owner] - [Effort] - [Priority]

Sign-Off:
Engineer: ********\_******** Date: **\_\_\_**
QA: **********\_\_********** Date: **\_\_\_**
Security: ******\_\_\_\_****** Date: **\_\_\_**
Tech Lead: ******\_\_\_****** Date: **\_\_\_**
PM: **********\_********** Date: **\_\_\_**

```

## Anti-Patterns

1. **Postmortem without root cause**
   - Mistake: "There was a null pointer exception, we added a null check, done"
   - Correct: Why was a null allowed? Why didn't tests catch this? Why didn't monitoring alert?
   - Fix: Ask "why" at least 5 times; find the root cause, not the symptom

2. **Improvements that don't match the root cause**
   - Mistake: Root cause is "engineer didn't understand the API contract", improvement is "add more unit tests"
   - Correct: If the issue is understanding, the improvement is better documentation or pair programming
   - Fix: Match improvement to root cause; ensure the improvement actually prevents recurrence

3. **Not closing the loop**
   - Mistake: "We identified improvements, but never added them to the backlog or implemented them"
   - Correct: Improvements must be backlog items with owners and timelines
   - Fix: PM must prioritize improvements into the backlog; track implementation

4. **Skipping the security review**
   - Mistake: "This was a minor bug, not a security incident"
   - Correct: Even minor bugs can have security implications; always review
   - Fix: Security reviews every incident; signs off on security impact

5. **Treating incidents as individual events**
   - Mistake: "This incident is unique; it doesn't suggest broader problems"
   - Correct: Incidents are signals of systemic weaknesses; look for patterns
   - Fix: Retro question: "Is this a new class of bug? Have we seen similar patterns?"

6. **Not including QA in root cause analysis**
   - Mistake: "Engineer posts-mortem, then we move on"
   - Correct: QA should be part of root cause (testing gaps) and improvements
   - Fix: QA required for incident review; QA owns improvements to test strategy

## Further Reading

- **Postmortem Culture** — Edmondson, A. M., "The Fearless Organization: Creating Psychological Safety in the Workplace for Learning, Innovation, and Growth" (Wiley)
- **Root Cause Analysis** — Niklas, C. D., "Root Cause Analysis Handbook" (Reliability Center, Inc.)
- **Incident Response** — PagerDuty, "Incident Response for Everyone" (https://response.pagerduty.com/)
```

---

## skills/retrospective-to-action/SKILL.md

```markdown
# Retrospective to Action

Runs a structured retro that produces actionable items assigned to specific roles.

## Domain Context

Retrospectives are where teams reflect on how they work and commit to changes. But many retros become venting sessions with no follow-up. The difference is structure: **retrospective-to-action** ensures that:

1. Observations are categorized (what went well, what didn't, what confused us)
2. Root causes are identified
3. Improvements are concrete and assigned
4. Improvements are prioritized and tracked

**ISO/IEC 12207 Reference:**

- 5.3.11 Disposal (lessons learned, process improvements)

## Instructions

### Step 1: Frame the Retro

- **Input**: Sprint context (dates, team size, key events)
- **Ask**: What were the sprint goals?
- **Ask**: Did we hit them? (yes/partial/no)
- **Ask**: What external events impacted the sprint? (incidents, stakeholder changes, dependency delays)

Output: Retro framing

### Step 2: Observations (What Happened)

Ask the team to capture observations in three categories:

**What Went Well?**

- Examples: "Code review process was smooth", "Feature shipped on time", "Team collaboration was great"
- Ask the team: What should we keep doing?

**What Didn't Go Well?**

- Examples: "Testing took longer than expected", "Communication with PM was unclear", "We shipped a bug to production"
- Ask the team: What should we change?

**What Confused Us?**

- Examples: "Requirements changed mid-sprint", "We weren't sure about the design until mid-development", "Monitoring didn't catch the issue"
- Ask the team: What would make this clearer next time?

Output: Observation lists from all team members

### Step 3: Root Cause Analysis

For each "didn't go well" observation:

- **Ask**: Why did this happen? (ask at least once)
- **Ask**: What could we have done differently?
- **Ask**: Is this a systemic issue or a one-time event?

Output: Root causes and potential improvements

### Step 4: Improvement Ideas

For each root cause:

- **Ask**: What's an improvement that would prevent this next sprint?
- **Ask**: Who would own this? (engineer, QA, PM, tech lead, designer)
- **Ask**: How much effort? (quick win, sprint work, epic work)

Output: Improvement ideas with owners and effort estimates

### Step 5: Prioritize Improvements

- **Quick wins** (< 1 hour) — Do this immediately
- **Sprint work** (1-5 days) — Add to next sprint backlog
- **Epic work** (> 5 days) — Plan for future sprint

- **High impact** — Affects multiple people, prevents recurrence of major issue
- **Medium impact** — Affects some people, improves efficiency
- **Low impact** — Nice to have, low urgency

Output: Prioritized improvement list

### Step 6: Assign Owners

For each improvement:

- **Ask**: Who should own this?
- **Ask**: Can they commit to doing it?

Output: Assignments

### Step 7: Consolidate Retro Notes
```

Retrospective: Sprint [N]
Date: [Date]
Team: [Members]

Sprint Goals: [Goals]
Goal Achievement: [Yes / Partial / No]
Key Events: [Incidents, blockers, changes]

What Went Well:
• [Observation] - Team sentiment: [positive]
• [Observation] - Team sentiment: [positive]
• [Observation] - Team sentiment: [positive]

What Didn't Go Well:
• [Observation] - Root cause: [cause] - Improvement: [idea]
• [Observation] - Root cause: [cause] - Improvement: [idea]
• [Observation] - Root cause: [cause] - Improvement: [idea]

What Confused Us:
• [Observation] - Clarity improvement: [idea]
• [Observation] - Clarity improvement: [idea]

Improvements (Prioritized):
Quick Wins (< 1 hour):
[ ] [Improvement] - Owner: [name] - Effort: [estimate]
[ ] [Improvement] - Owner: [name] - Effort: [estimate]

Sprint Work (1-5 days):
[ ] [Improvement] - Owner: [name] - Effort: [estimate]
[ ] [Improvement] - Owner: [name] - Effort: [estimate]

Epic Work (> 5 days):
[ ] [Improvement] - Owner: [name] - Effort: [estimate]

Backlog Items Created:
[ ] [Issue ID] - [Title] - [Owner] - [Sprint]
[ ] [Issue ID] - [Title] - [Owner] - [Sprint]
[ ] [Issue ID] - [Title] - [Owner] - [Sprint]

Follow-Up:
Next retro date: [Date]
Review improvement status: [Yes / No]

```

### Step 8: Close the Loop

- **Immediately** — Assign quick wins; expect completion in next 1-2 days
- **Next Sprint** — Prioritize sprint work improvements into backlog
- **Future** — Track epic work improvements; plan for dedicated sprint

## Anti-Patterns

1. **Retros without follow-up**
   - Mistake: "We did retro, identified improvements, but never tracked them"
   - Correct: Every improvement becomes a backlog item with an owner
   - Fix: PM is responsible for creating and tracking improvement backlog items

2. **Retros that blame individuals**
   - Mistake: "Engineer shipped a bug, so they should write better code"
   - Correct: Bugs are systemic failures (inadequate testing, unclear requirements, design issues)
   - Fix: Focus on systemic improvements, not individual blame

3. **Treating all observations as equally important**
   - Mistake: "All 15 observations get equal discussion time"
   - Correct: Prioritize by impact and effort
   - Fix: Time-box the retro (60 min for week sprint); prioritize top 5 improvements

4. **Improvements with no owners**
   - Mistake: "We should improve testing" (but no one owns it)
   - Correct: Every improvement has a specific owner who commits to it
   - Fix: In retro, ask: "Who owns this? Can you commit to next sprint?"

5. **Not reviewing improvement status in the next retro**
   - Mistake: "We created improvement items, but never checked if they happened"
   - Correct: Start next retro by reviewing previous sprint's improvements
   - Fix: Retro agenda always includes: "Review last sprint's improvement items"

6. **Retros that don't inform process changes**
   - Mistake: "We retro, but DoR/DoD never change based on learnings"
   - Correct: Retros should evolve team practices
   - Fix: Question: "Should this observation change our DoR or DoD?"

## Further Reading

- **Agile Retrospectives** — Derby, E. & Larsen, D., "Agile Retrospectives: Making Good Teams Great" (Pragmatic Bookshelf)
- **Blameless Postmortems** — Edmonson, A., "The Fearless Organization" (Wiley)
- **Continuous Improvement** — Liker, J., "The Toyota Way" (McGraw-Hill)
```

---

## skills/compliance-traceability/SKILL.md

```markdown
# Compliance Traceability

Maps requirements → design decisions → implementation → tests → deployment to ensure full traceability for regulated environments.

## Domain Context

In regulated industries (finance, healthcare, aviation, automotive), you must be able to prove:

- This requirement is implemented (not forgotten)
- This code change is justified (required by a requirement or architectural decision)
- This test validates a requirement (test coverage is intentional, not accidental)
- This deployment includes all required changes (no partial rollouts)

Traceability is the audit trail that proves the system was built intentionally, not accidentally.

**Relevant Standards:**

- **ISO/IEC 12207** — Software lifecycle processes (5.2.1 Requirements, 5.3 Technical processes)
- **ISO/IEC 15288** — Systems and software engineering lifecycle
- **FDA 21 CFR Part 11** — Electronic records for regulated industries
- **SOC 2** — Compliance for cloud services
- **HIPAA** — Healthcare data protection
- **PCI-DSS** — Payment card security

## Instructions

### Step 1: Establish Traceability Scope

- **Input**: Regulation/framework (e.g., SOC 2, HIPAA, PCI-DSS), feature or system to audit
- **Ask**: What requirements must be traced?
- **Ask**: What's the risk level? (critical infrastructure, customer data, financial systems)

Output: Traceability scope

### Step 2: Requirements Definition

Document each requirement:
```

Requirement ID: REQ-001
Title: [Descriptive title]
Description: [What must be true?]
Regulatory Reference: [Which regulation requires this?]
Risk if not met: [What happens if we don't do this?]
Status: [New / Designed / Implemented / Verified / Deployed / Closed]

```

Output: Requirements registry

### Step 3: Design Decisions

For each requirement, document design decisions:
```

Requirement: REQ-001
Design Decision: DD-001
Title: [How we decided to implement this]
Alternatives Considered: [Other approaches we considered]
Trade-offs: [Why we chose this over alternatives]
Architecture Diagram: [Link to diagram showing how this works]
Risk Mitigation: [How we address risks]
Status: [Approved / Rejected / Revised]

```

Output: Design decision registry

### Step 4: Implementation

For each design decision, document code:
```

Design Decision: DD-001
Implementation: IMPL-001
Code Component: [File, function, class]
Code Location: [GitHub link with line numbers]
Implementation Notes: [How does this code implement the design decision?]
Code Review: [PR link, reviewer approval]
Status: [In Progress / Complete / In Testing / In Deployment]

```

Output: Implementation registry

### Step 5: Test Coverage

For each implementation, document tests:
```

Implementation: IMPL-001
Test: TEST-001
Test Type: [Unit / Integration / System / Acceptance / Compliance]
Test Scope: [What does this test validate?]
Test Location: [File, test class, test name]
Expected Result: [What should happen?]
Actual Result: [Does it pass?]
Test Status: [Passing / Failing / Skipped]
Evidence: [Test run logs, coverage report]

```

Output: Test evidence registry

### Step 6: Deployment Verification

For each test, verify deployment:
```

Test: TEST-001
Deployment: DEPLOY-001
Environment: [Dev / Staging / Production]
Deployment Checklist:
[ ] Code merged to main branch
[ ] Tests passing in CI/CD
[ ] Security scans passed
[ ] Performance baselines met
[ ] Monitoring configured
[ ] Rollback plan ready
Deployment Date: [Date]
Deployment Evidence: [Build logs, deployment ticket, monitoring alerts]
Status: [Deployed / Reverted / On Hold]

```

Output: Deployment evidence

### Step 7: Create the Traceability Matrix

Build a matrix showing requirements → design → implementation → tests → deployment:

```

Requirement | Design Decision | Implementation | Tests | Deployment | Status
REQ-001 | DD-001 | IMPL-001 | TEST-001 | DEPLOY-001 | Complete
REQ-002 | DD-002 | IMPL-002 | TEST-002 | DEPLOY-002 | Complete
...

Coverage Statistics:
Requirements Traced: [N / N]
Design Decisions Documented: [N / N]
Code Covered by Tests: [N%]
All Tests Passing: [Yes / No]
All Deployed to Production: [Yes / No]
Traceability Gap: [%]

```

Output: Traceability matrix

### Step 8: Identify Gaps

For each unmapped item:
- **Orphaned Requirement** — Requirement with no design/implementation
- **Orphaned Code** — Code not required by any requirement
- **Missing Tests** — Implementation with no tests
- **Undeployed Changes** — Tested code not yet deployed

Output: Gap analysis with remediation plan

### Step 9: Compliance Sign-Off

```

Traceability Audit: [Feature / System]
Date: [Date]
Auditor: [PM / Tech Lead]
Framework: [SOC 2 / HIPAA / PCI-DSS / etc.]

Coverage:
[ ] All requirements traced to design decisions
[ ] All design decisions traced to implementation
[ ] All implementation traced to tests
[ ] All tests traced to deployment
[ ] No orphaned requirements
[ ] No orphaned code
[ ] Code coverage >= [threshold]%
[ ] All tests passing
[ ] All code deployed to production

Compliance Status:
[ ] Compliant — All items traced and verified
[ ] Conditionally Compliant — [List gaps, remediation plan]
[ ] Non-Compliant — [List critical gaps, must fix before release]

Auditor Sign-Off:
Audit Owner: ******\_****** Date: **\_\_\_**
Remediation Owner: **\_\_\_** Date: **\_\_\_**

```

## Anti-Patterns

1. **Traceability added after the fact**
   - Mistake: "We shipped the feature, now let's add traceability"
   - Correct: Traceability is part of the design process, not an afterthought
   - Fix: Traceability starts in requirements phase; maintained during design/implementation

2. **Traceability for documentation, not for confidence**
   - Mistake: "We have a traceability matrix for compliance, but no one actually uses it"
   - Correct: Traceability should help you answer: "Will this requirement be tested before deployment?"
   - Fix: Traceability matrix should be a living document; reviewed before each deployment

3. **Orphaned code (code with no requirement)**
   - Mistake: "We added a feature, but it wasn't in the original requirements"
   - Correct: If code exists, there should be a requirement that justifies it
   - Fix: When unplanned code appears, retroactively create the requirement

4. **Over-tracing (creating artifacts that aren't real)**
   - Mistake: "Traceability matrix requires 10 documents per requirement, so we created dummy documents"
   - Correct: Traceability should reflect actual work; don't create fake artifacts
   - Fix: Trace only what's real; adjust documentation burden to match team capacity

5. **Not catching traceability gaps during development**
   - Mistake: "We discover gap analysis at audit time"
   - Correct: Traceability gaps should be caught before deployment
   - Fix: Pre-deployment checklist includes: "Run traceability audit, verify no gaps"

## Further Reading

- **ISO/IEC 12207** — Software lifecycle processes (Section 5, Lifecycle processes)
- **FDA Software Validation** — https://www.fda.gov/regulatory-information/search-fda-guidance-documents/part-11-electronic-records-electronic-signatures
- **Requirements Traceability** — IEEE 830, IEEE 1028
- **Compliance Frameworks** — https://www.aicpa.org/interestareas/informationtechnology/researcha-and-guidance/aicpa-soc-2-criteria
```

---

## commands/design-review.md

```markdown
---
description: Run a cross-functional design review combining architecture, security, and UX perspectives.
argument-hint: "[feature or system to review]"
---

Conduct a comprehensive design review combining perspectives from architecture, security, and UX.

## Steps

1. **Architect System Decomposition** (`architect:system-decomposition`)
   - Input: Feature description or system boundary
   - Output: Component diagram, dependency list, scaling implications

2. **Security Threat Analysis** (`security:stride-analysis`)
   - Input: Architecture from step 1
   - Output: Threat model, attack vectors, required mitigations

3. **UX Accessibility Review** (`designer:accessibility-audit`)
   - Input: Design mockups or design spec
   - Output: Accessibility assessment (WCAG 2.1 compliance), interaction patterns

4. **Architect Trade-Off Analysis** (`architect:trade-off-analysis`)
   - Input: Concerns from security and UX reviews
   - Output: Documented trade-offs (performance vs. security, simplicity vs. flexibility)

5. **Synthesis**
   - Consolidate: Architecture + Threats + Accessibility + Trade-offs
   - Verdict: Approved / Conditional / Rejected with required changes
   - Risk Assessment: Severity and mitigation for remaining risks

## Output

**Design Review Report** containing:

- Approved design verdict
- Required changes (if conditional)
- Risk assessment with severity levels
- Remaining risks and their mitigations
- Recommendation: Ready for implementation or needs redesign

## Example
```

/sdlc:design-review "User authentication system redesign"

→ Architect decomposes: OAuth2 + JWT + session store
→ Security identifies: Token refresh strategy, session hijacking risks
→ Designer reviews: Login flow accessibility, mobile responsiveness
→ Architect documents: Security vs. complexity trade-offs
→ Synthesis: Approved with condition: "Session timeout must be ≤ 30 min"

Output: Design Review Report
Verdict: CONDITIONAL APPROVAL
Required Changes: Session timeout configuration
Risk Assessment: [table of risks, severity, mitigations]

```

## Next Steps

After design review approval, suggest:
- "Ready to move to `/sdlc:quality-gate` to verify the design meets phase gate criteria."
- "Create implementation tickets for any conditional changes."
```

---

## commands/feature-kickoff.md

```markdown
---
description: Launch a feature with full cross-functional alignment and DoR validation.
argument-hint: "[feature name or epic name]"
---

Conduct a feature kickoff that aligns all teams and validates Definition of Ready.

## Steps

1. **PM Problem Statement** (`product-manager:problem-statement`)
   - Input: Feature request or roadmap item
   - Output: Problem statement, success metrics, user impact

2. **PM PRD Template** (`product-manager:prd-template`)
   - Input: Problem statement
   - Output: Product requirements document (user stories, acceptance criteria, constraints)

3. **Architect System Decomposition** (`architect:system-decomposition`)
   - Input: Requirements from PRD
   - Output: Component diagram, complexity assessment, feasibility

4. **Security Threat Analysis** (`security:stride-analysis`)
   - Input: System decomposition
   - Output: Threat model, required security controls

5. **Tech Lead Work Breakdown** (`tech-lead:work-breakdown-structure`)
   - Input: Requirements + Threats + Architecture
   - Output: Work breakdown structure with effort estimates

6. **Definition of Ready Validation** (`sdlc:definition-of-ready`)
   - Input: All artifacts from above
   - Output: DoR checklist with approval

## Output

**Kickoff Summary** containing:

- Aligned team understanding of the problem
- Acceptance criteria everyone agrees on
- Architecture that the team commits to
- Security assumptions and requirements
- Work breakdown with effort estimates
- Definition of Ready checklist

## Example
```

/sdlc:feature-kickoff "Multi-factor authentication"

→ PM articulates: "Users need MFA to reduce account takeovers"
→ PRD defines: Support SMS + TOTP, allow user to disable, fallback via email
→ Architect proposes: Service + factor provider abstraction
→ Security identifies: Session invalidation on MFA setup, rate limiting, backup codes
→ Tech Lead estimates: 8 days (architect), 16 days (backend), 10 days (frontend), 5 days (QA), 2 days (security)
→ DoR validates: All criteria met

Output: Kickoff Summary
Problem: Reduce account takeovers via MFA
Success Metric: 80% user adoption within 3 months
Architecture: Service + provider abstraction
Security Assumptions: [list]
Work Breakdown: [timeline]
DoR Status: APPROVED

```

## Next Steps

- "Feature is ready for sprint planning. Add all work breakdown items to the backlog."
- "Schedule architecture review before development starts."
```

---

## commands/pre-launch-review.md

```markdown
---
description: Comprehensive pre-release validation across all roles.
argument-hint: "[feature set or release version]"
---

Run a full pre-release validation across PM, QA, security, and operations.

## Steps

1. **Release Checklist** (`sdlc:release-checklist`)
   - Input: Feature set, release notes, deployment plan
   - Output: Release readiness checklist

2. **Engineer Deployment Strategy** (`engineer:deployment-strategy`)
   - Input: Features to deploy
   - Output: Deployment plan, rollback procedure, risk assessment

3. **Security Penetration Test Scope** (`security:penetration-test-scope`)
   - Input: Features to deploy
   - Output: Pen testing plan, scope, success criteria

4. **QA Release Readiness** (`qa:release-readiness-assessment`)
   - Input: Features, test results, known issues
   - Output: Test completion status, risk assessment

5. **PM Launch Readiness** (`product-manager:launch-readiness-checklist`)
   - Input: Features, success metrics, communications
   - Output: Launch readiness, customer communications, stakeholder approval

## Output

**Pre-Launch Report** containing:

- Release readiness matrix (PM, QA, security, engineer, tech lead sign-offs)
- Test completion and coverage
- Known issues and severity assessment
- Deployment and rollback procedures
- Launch communication plan
- Blocking issues (if any)

## Example
```

/sdlc:pre-launch-review "Payment redesign v2.0"

→ Release Checklist: 95% pass (1 warning on performance)
→ Deployment Strategy: Blue-green with 15-minute cutover
→ Security Pen Test: Scope approved, scheduled for tomorrow
→ QA: All test cases passing, 98% coverage, 3 known low-severity bugs
→ PM: Launch communication ready, stakeholder approval obtained

Output: Pre-Launch Report
Release Ready: YES (subject to pen test completion)
Test Coverage: 98%
Blocking Issues: None
Warnings: Performance testing shows 5% regression under load
Sign-offs: [PM ✓] [Engineer ✓] [QA ✓] [Security ⏳] [Tech Lead ✓]

```

## Next Steps

- "Pen test results received, approve or address findings."
- "Ready to deploy to production." OR "Address warnings before deploying."
```

---

## commands/security-review.md

```markdown
---
description: Deep security review of design and implementation.
argument-hint: "[feature name, component, or system to review]"
---

Conduct a comprehensive security review covering design threats, code vulnerabilities, and dependency risks.

## Steps

1. **Security Secure Coding Review** (`security:secure-coding-review`)
   - Input: Code to review
   - Output: Secure coding violations, best practices

2. **Security OWASP Top 10 Check** (`security:owasp-top-ten-check`)
   - Input: Application features (web/API)
   - Output: OWASP Top 10 risks assessment

3. **Security Dependency Vulnerability Scan** (`security:dependency-vulnerability-scan`)
   - Input: Dependency list
   - Output: CVE scan results, remediation plan

4. **Architect Security Architecture** (`architect:security-architecture`)
   - Input: System design
   - Output: Security architecture, control points, defense in depth

## Output

**Security Review Report** containing:

- Secure coding violations and fixes
- OWASP Top 10 risk assessment
- Dependency vulnerabilities and remediation
- Security architecture approval
- Overall security posture and recommendations

## Example
```

/sdlc:security-review "User data export feature"

→ Code Review: 2 SQL injection risks identified (fixed)
→ OWASP Check: A01 Broken Access Control risk (users can export other users' data)
→ Dependency Scan: 3 high CVEs in xlsx library (upgrade available)
→ Security Architecture: Export should be rate-limited, logged, signed

Output: Security Review Report
Secure Coding: 2 issues found, all fixed
OWASP Risks: A01 Access Control (CRITICAL), A04 Insecure Design (MEDIUM)
Vulnerabilities: 3 high CVEs, 2 medium CVEs (plan to upgrade xlsx)
Architecture Approval: CONDITIONAL (requires rate limiting and audit logging)
Overall: NOT APPROVED — Fix access control issue before deployment

```

## Next Steps

- "Address critical security issues before submitting for quality gate."
- "Submit updated code for re-review."
```

---

## commands/quality-gate.md

```markdown
---
description: Validate work meets all quality standards before advancement.
argument-hint: "[feature name or work item]"
---

Run a comprehensive quality gate that validates work across code quality, test coverage, security, and operations readiness.

## Steps

1. **SDLC Phase Gate** (`sdlc:sdlc-phase-gate`)
   - Input: Current work phase, work item
   - Output: Phase gate decision (go/conditional/no-go)

2. **Definition of Done** (`sdlc:definition-of-done`)
   - Input: Completed work
   - Output: DoD checklist with role sign-offs

3. **QA Test Coverage Analysis** (`qa:test-coverage-analysis`)
   - Input: Code and test results
   - Output: Coverage report, gap analysis

4. **Engineer Code Complexity** (`engineer:code-complexity-analysis`)
   - Input: Code changes
   - Output: Complexity assessment, refactoring recommendations

5. **Security Scan** (`security:security-scan`)
   - Input: Code and dependencies
   - Output: Vulnerability report, remediation plan

## Output

**Quality Gate Report** containing:

- Phase gate verdict
- DoD checklist status
- Test coverage analysis
- Code complexity assessment
- Security scan results
- Overall go/no-go decision with blocking issues

## Example
```

/sdlc:quality-gate "Search filtering feature"

→ Phase Gate: Ready to move from Development to Testing
→ DoD: 8/10 criteria met (missing performance baseline, accessibility test)
→ Test Coverage: 92% (above 85% threshold)
→ Code Complexity: Cyclomatic complexity 8 (acceptable)
→ Security Scan: 0 critical, 2 medium (remediation plan in progress)

Output: Quality Gate Report
Phase Gate: CONDITIONAL (requires DoD completion)
Test Coverage: PASS (92%)
Code Complexity: PASS
Security: CONDITIONAL (address 2 medium issues)
Overall: NOT READY — Complete DoD and security remediation
Blocking Issues: [list]

```

## Next Steps

- "Fix DoD gaps (performance baseline, accessibility test)."
- "Address security medium-risk issues."
- "Re-run quality gate when complete."
```

---

## commands/full-lifecycle.md

```markdown
---
description: Trace a feature from problem statement through post-deployment monitoring.
argument-hint: "[feature name or epic name]"
---

Map a feature's complete lifecycle from problem definition through production stability.

## Steps

1. **PM Problem Statement** (`product-manager:problem-statement`)
   - Output: Problem, success metrics

2. **Architect System Decomposition** (`architect:system-decomposition`)
   - Output: Architecture, components, scaling plan

3. **Definition of Ready** (`sdlc:definition-of-ready`)
   - Output: DoR validation

4. **Engineer Test Strategy** (`engineer:test-strategy`)
   - Output: Unit, integration, and system test plans

5. **Security Threat Analysis** (`security:stride-analysis`)
   - Output: Threat model, required controls

6. **QA Test Strategy** (`qa:test-strategy-design`)
   - Output: QA test cases, coverage plan

7. **Definition of Done** (`sdlc:definition-of-done`)
   - Output: DoD validation

8. **Release Checklist** (`sdlc:release-checklist`)
   - Output: Pre-deployment validation

## Output

**Feature Lifecycle Document** containing:

- Requirements → Design → Implementation → Testing → Deployment → Operations timeline
- Linked artifacts at each phase (PRD, architecture, code, tests, deployment plan)
- Role contributions and sign-offs
- Metrics and monitoring strategy
- Post-launch actions and follow-ups

## Example
```

/sdlc:full-lifecycle "Dark mode support"

→ PM: "Reduce eye strain for evening users" (success: 40% adoption in 1 month)
→ Architect: CSS custom properties + system preference detection + storage
→ DoR: Requirements, design, feasibility approved
→ Engineer: Unit tests for preference logic, system tests for UI
→ Security: No threats identified, no auth changes
→ QA: Visual regression tests on 5 target devices
→ DoD: All tests passing, accessibility verified, docs updated
→ Release: Deployment to 10% users, monitor for CSS issues

Output: Feature Lifecycle Document
[Timeline from problem → production]
[Linked artifacts]
[Role contributions]
[Monitoring dashboard links]

```

## Next Steps

- "Feature is ready for next phase: [Design / Development / Testing / Deployment / Operations]."
- "Review lifecycle document with team; update as you progress."
```

---

## commands/post-incident-review.md

```markdown
---
description: Transform an incident into systemic improvements across all teams.
argument-hint: "[incident name or ID]"
---

Conduct a thorough incident review that transforms the incident into lasting improvements.

## Steps

1. **Engineer Postmortem** (`engineer:postmortem-analysis`)
   - Input: Incident timeline, resolution steps
   - Output: What happened, how it was resolved

2. **Security Root Cause** (`security:root-cause-analysis-security`)
   - Input: Incident details
   - Output: Security impact, data compromise assessment

3. **Incident to Improvement** (`sdlc:incident-to-improvement`)
   - Input: Postmortem + security assessment
   - Output: Improvement recommendations by role

4. **Tech Lead Process Improvement** (`tech-lead:process-improvement`)
   - Input: Improvements, team capacity
   - Output: Implementation plan, timeline

5. **PM Backlog Prioritization** (`product-manager:prioritize-backlog`)
   - Input: Improvement items
   - Output: Prioritized backlog with owners

## Output

**Post-Incident Review Report** containing:

- Incident narrative and timeline
- Root cause analysis
- Security impact assessment
- Improvement recommendations by role
- Prioritized backlog items with owners and timelines
- Sign-offs from engineer, QA, security, tech lead, PM

## Example
```

/sdlc:post-incident-review "Payment processing timeout"

→ Postmortem: Timeout occurred because external payment API changed response time
→ Security: No data compromise, but rate-limiting was not enforced
→ Improvements: - Add circuit breaker to payment service (engineer, 8 hours) - Rate-limit external API calls (engineer, 4 hours) - Add timeout alerting (tech lead, 2 hours) - Add chaos testing for external dependencies (QA, 16 hours)
→ Backlog: 4 items prioritized for next sprint

Output: Post-Incident Review
Incident: Payment processing timeout affecting 500 customers for 8 minutes
Root Cause: External API response time increased from 50ms to 2s; no circuit breaker
Security Impact: None
Improvements: [list with owners and effort]
Backlog Items: [created and prioritized]
Sign-offs: [engineer] [QA] [security] [tech lead] [PM]

```

## Next Steps

- "Backlog items created and ready for sprint planning."
- "Follow up in 1 month to verify improvements were implemented."
```

---

## commands/sprint-ceremony.md

```markdown
---
description: Conduct a complete sprint cycle with planning, DoR validation, retrospective, and action items.
argument-hint: "[sprint number or date range]"
---

Facilitate a comprehensive sprint cycle: planning, DoR validation, retrospective, action items.

## Steps

1. **Tech Lead Sprint Planning** (`tech-lead:sprint-planning-guide`)
   - Input: Backlog, team capacity, dependencies
   - Output: Sprint goal, sprint backlog, team commitments

2. **Definition of Ready Validation** (`sdlc:definition-of-ready`)
   - Input: Sprint backlog items
   - Output: DoR checklist for each item; identify items not ready

3. **Retrospective to Action** (`sdlc:retrospective-to-action`)
   - Input: Previous sprint observations, metrics
   - Output: Improvements and action items

4. **PM Backlog Prioritization** (`product-manager:prioritize-backlog`)
   - Input: Backlog, improvements from retro
   - Output: Next sprint priorities

## Output

**Sprint Ceremony Summary** containing:

- Sprint goal and planned items
- DoR validation (items ready / not ready)
- Retrospective observations (what went well, what didn't)
- Action items with owners and timelines
- Priorities for next sprint
- Team commitments and capacity

## Example
```

/sdlc:sprint-ceremony "Sprint 42 (Mar 11 - Mar 22)"

→ Planning: 8 items planned, 75 story points, 3 team members
→ DoR Validation: 7/8 items ready; 1 item needs design review (defer to Sprint 43)
→ Retrospective: Positive: "Code reviews fast", Negative: "Design phase bottleneck"
→ Actions: "Add designer to code review" (tech lead, low effort)
→ Priorities: "Design is now critical path; add designer capacity"

Output: Sprint Ceremony Summary
Sprint Goal: "Complete authentication redesign"
Sprint Backlog: [7 items, 71 story points]
DoR Status: 7/8 items ready
Retrospective Actions: [list with owners]
Next Sprint Priorities: [list]
Team Confidence: HIGH

```

## Next Steps

- "Sprint starts tomorrow at 9:00am."
- "Defer 1 not-ready item; prioritize for Sprint 43."
- "Implement retrospective action: add designer to code review."
```

---

This completes the SDLC plugin structure. All files are ready to copy to `/Users/sethford/Documents/claude-skills/sdlc/`.

```

```
