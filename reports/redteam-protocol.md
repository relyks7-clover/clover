# Red-Team Protocol v1.0

## 0. Scope and Purpose

This document defines a repeatable, gate-controlled protocol for adversarial red-teaming of software, services, and operations. It is OODA-framed, evidence-grounded, and derived from both normative security doctrine (MITRE ATT&CK, NIST SP 800-115, CVSS v3.1) and lessons from failures observed in live delivery/test operations.

The protocol is the same regardless of target class: web application, API, workflow, protocol, automation, supply chain, or internal process. Goal is not to "break" a target but to demonstrate *specific, reproducible, residual risk* with evidence sufficient to trigger a corrective decision.

Hard constraints (non-negotiable):
- Authorization must be written, in-scope, and current. No authorization means no testing.
- Operations must be passive-first, active-second; escalation is gated by evidence and authority.
- Every assertion in a finding must be byte-level verifiable in the delivered report. Inference is labeled inference, never proof.
- Destructive or state-changing action requires an explicit decision gate, not convenience.
- Engagement memory is stored before action, audited after, and never overwritten.

---

## 1. Core Doctrine

### 1.1 The loop

The protocol is a closed decision loop. Each cycle (Observe → Orient → Decide → Act) consumes a phase, pauses at gates, and moves no faster than evidence.

```
(Observe) -> (Orient) -> (Decide) -> (Act)
    ^                                  |
    +--------------<------------------+
```

Definitions of each operation in the red-team context:

- Observe. Collect what is *true* of the target surface. Sources: passive recon, public indexes, archival dumps, traffic capture, source mirrors, error diffs, TLS metadata. Rule: collect raw, timestamped, hashed. Do not summarize on capture; summarize after.
- Orient. Filter real signal from ambient noise. Classify each observation by relevance to the engagement goal, triple-check assumptions of reachability and impact before they become premises. This is where most failures happen: a hypothesis treated as fact because it is plausible.
- Decide. Pick a single action (or a stop). Every decision references the engagement objective. A decision with no link back to the objective is noise. Severity is not the order of interest; exploitability x impact x persistence is.
- Act. Execute the chosen action. Record the result unmodified. If the action's outcome is *not* reproducible, it is not a finding; it is a hypothesis awaiting confirmation.

### 1.2 Centers of gravity

Each engagement maps to a defined target objective — an asset or behavior whose compromise constitutes the engagement "win." All actions must advance toward one of:
- Integrity: attacker-controlled modification of data or state without authorization
- Confidentiality: disclosure of protected data to an unprivileged actor
- Availability: denial or degradation of an expected service
- Trust: compromise of verification or authentication chain
- Operations: unauthorized execution, persistence, or lateral movement

The report maps each finding to exactly one center of gravity. A finding that maps to none is out-of-scope noise.

### 1.3 Decision gates

The following gates are traversed in order, and a phase does not begin until the previous gate asserts with evidence:

1. Authorization gate. Scope written, assets enumerated, rules of engagement signed, emergency contacts set.
2. Passive gate. No direct interaction with target beyond what is documented and permitted. Only public metadata, cached artifacts, standard fingerprinting.
3. Active gate. Direct interaction begins. Every interaction is logged. Write actions (e.g., booking an account, mutating state) are flagged.
4. Exploit gate. A candidate vulnerability is being *validated* with permission. The minimum payload required to prove impact is used, and cleanup is planned before it is used.
5. Persistence gate. Only when the engagement explicitly asks for persistence. Otherwise, proof of access is enough; no implant is installed.
6. Reporting gate. Every finding is reproduced a minimum of twice: once against the test build, once against current public version. Results recorded as raw output.

---

## 2. Engagement Lifecycle

### 2.1 Pre-engagement (Day -1 to 0)

Actions:
- Form the Target Card: target, asset list, authorized hosts, authorized techniques, test window, emergency contact.
- Record the Authorization Reference: signed document, number, or ticket from the commissioning party.
- Set the Reporting Channel: where recon notes, evidence, and the final report reside; retention constraints.
- Choose the Tool Retainer: exact tool versions, with checksums. Never an unrecognized binary.
- Baseline the target: capture current version, build hash, environment banner before any interaction.

Gate exit: written authorization exists, target card complete, tool checksums recorded.

### 2.2 Observe

Passive data collection. No packet sent to the target that changes behavior.
- DNS, WHOIS, TLS metadata, search indexes, cached snapshots, archived pages.
- Error diffs and public changelogs (e.g., identifying feature/version deltas that alter attack surface).
- Auth flows, route arrays, headers, cookies, and layout as seen by an unauthenticated client.

Recording standard: observations are stored in raw form, annotated with UTC timestamp, captor tool, hash, and source URL if any.

### 2.3 Orient

Hypothesis management:
- List candidate assumptions about the target. Classify each as *observed*, *inferred*, or *asserted*.
- *Observed*: directly captured. Highest trust.
- *Inferred*: derived from context but not yet captured. Needs confirmation.
- *Asserted*: from docs or third party. Needs verification.
- No attack is designed from an *asserted* premise. Confirmation of any premise is an Observe step, not a leap.

Output of Orient: a prioritized list of candidate surface/tool pairs, each with a precondition chain (what must be true for this to matter) and the cheapest evidence that would test it.

### 2.4 Decide

Select one candidate. Selection rule: highest expected *demonstrable impact* per unit of authorized effort, filtered by the Rules of Engagement. If none meet bar, stop; report the target's resistance model as a result, not a failure.

### 2.5 Act

Interact with target surface. For every interaction:
- Record the tool, exact command, token/throttle, time, and response.
- On any unexpected error, stop the current action and orient before continuing.
- Once a behavioral delta is confirmed, minimize to a reproducible proof.

---

## 3. Operations in Depth: The Attack Path

### 3.1 Recon to Trust Discovery (Observe + Orient)
- Identify all entry vectors within authorized scope: network, API, web, mail, app stores, supply chain, third-party integrations.
- Document authentication state, trust boundaries, and privilege roles visible from outside.
- Identify input channels: forms, JSON, file upload, XML, LDAP, SMTP, MCP.
- Identify target platforms: language, framework, runtime, patch level, browsers.

### 3.2 Vulnerability Hypothesis (Orient)
For each input channel, map attacker-controlled data to sink. Use checklist of common sink classes
- SQL, NoSQL, command, eval, file include, deserialization, SSRF, redirect, template injection, header injection, path traversal, XSS (reflected / stored / DOM), CSRF, auth bypass, IDOR, privilege escalation, XML external entity (XXE), time-of-check/time-of-use.

### 3.3 Validation (Act + Decide)
- Passive hypothesis: observe behavior under benign input, confirm sink response.
- Minimally active: send one controlled probe that proves sink use without destructive effect.
- Entirely active: only with specific authorization for the target asset; use a non-destructive payload; capture raw response.

Every confirmed finding must contain:
- Exact request (with all headers/body) and exact response snippet.
- Notes explaining why the sink is vulnerable (reference to code path, parameter flow).
- Impact in the context of the center of gravity.
- Replication command and reproducible environmental conditions.

---

## 4. Rules of Engagement (defaults)

- No destructive action on production unless explicitly authorized by the commissioning party for that asset.
- No data exfiltration beyond what is required to prove impact; when proof demands it, capture a minimal sample and redact.
- No lateral movement beyond authorized scope; if discovered inadvertently, stop, report, and request scope extension.
- No persistence unless explicitly requested. If persistence is accidentally created, remove it immediately and note the path.
- No social engineering of people unless the engagement explicitly includes phishing and prior approval is on the Target Card.
- Reporting is expected within an agreed window; findings are reported as soon as triage confirms they are unique and real.
- Follow the disclosure protocol on the Target Card; no public disclosure without authorization.

---

## 5. Evidence Integrity and Reporting

### 5.1 Evidence handling
- Every artifact is retained as raw bytes, hashed (SHA-256 at collection), and attributed.
- Analysis never transforms the raw capture; it references it.
- If a tool's output is post-processed, the raw output is attached with the processing step described.
- All times UTC. All paths absolute.

### 5.2 Report structure

Report template (markdown):

```markdown
# Red-Team Report: <target>
# Engagement ID <id> | Date <UTC> | Authorization <ref>

## Executive summary
<2-3 paragraphs: what we tested, what we proved, residual risk, priority>

## Scope and authorization
<assets, techniques, window, contact>

## Method
<OODA phases summarized, tools, gates>

## Findings
### F1. <title>
- Severity: <CVSS v3.1 score + vector>
- Center of gravity: <integrity/confidentiality/availability/trust/operations>
- Evidence: <raw capture refs>
- Reproduction: <exact steps>
- Impact: <real consequence, not theoretical>
- Fix guidance: <specific, actionable>
- Status: <open/confirmed/validated/duplicate>

## Resistance model
<what we tried that did not yield; why the target held>

## Non-findings
<observed anomalies not in scope, or hypotheses not confirmed>

## Appendix: Raw logs, target baseline, tool checksums
```

---

## 6. Operational Lessons Appended to This Protocol

This protocol codifies known failure modes from prior operations. Each rule above traces to a concrete lesson:

1. Never assert an assumption as fact. In an earlier delivery, a version-specific header was called the root cause before it was byte-proven; the actual trigger remained inferred. Rule: any finding that is asserted must be reproduced end-to-end.
2. Tool-call shape matters: a mis-encoded header or wrong method burns a turn and corrupts the operation. Rule: in live operations, every external call is validated for format before sending.
3. Exact PIDs, not regex kill patterns. A broad kill pattern matched the running shell and killed the operator's own process. Rule: never terminate a process by pattern when a PID is available.
4. Wrong API envelope. Submitting with `artifacts: {}` when the API requires `artifact_refs: []` produced a 400 and a retry loop. Rule: always fetch and validate a schema before writing to an external contract.
5. Test on the production path, not a replica. A feature validated only against a shadow implementation proves nothing. Rule: any validated capability must be exercised against the exact production surface.
6. Read raw result, not summary. Tool output can be truncated or transformed; a summary is not evidence. Rule: raw output is the only admissible evidence.

---

## 7. Appendix: Field Checklist

Pre-mission:
- [ ] Authorization doc present, in scope, current
- [ ] Target card complete, emergency contacts set
- [ ] Tools pinned with checksums
- [ ] Recording channel active
- [ ] Baseline captured

Each phase:
- [ ] Gate conditions met
- [ ] Raw capture retained
- [ ] Hypothesis state documented (observed/inferred/asserted)
- [ ] Decision references objective
- [ ] Cleanup verified

Post-mission:
- [ ] All findings reproduced x2
- [ ] Report structured to template
- [ ] Data retention honored
- [ ] Debrief with commissioning party
