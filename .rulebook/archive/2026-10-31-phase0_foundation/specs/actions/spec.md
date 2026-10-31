# Actions Specification

## ADDED Requirements

### Requirement: Action Contract
Every change MyClean can make MUST be implemented as an action that provides describe (what, why, risk, tier, execution context svc or user), preview (exact items, bytes, or setting old and new values), apply (re-validates, executes and returns undo data), verify (re-measures the targeted metric) and undo (reverses the change or states that it is not reversible and why, such as "regenerates").

#### Scenario: Preview matches apply
Given an action whose preview lists 3 items totalling 1.2 GB
When the action is applied with that preview
Then apply acts on no item outside those 3
And the returned undo data covers each item it changed

#### Scenario: Irreversible action declares it
Given a `DeleteItems` action on regenerable cache files
When its undo is requested
Then it returns "not reversible: regenerates" instead of failing silently

### Requirement: Plan State Machine
Plans MUST move only along `proposed → (auto | awaiting_approval) → approved → applying → applied → (verified | rolled_back | failed)`, with `failed` also reachable from `applying`, and every other transition SHALL be rejected.

#### Scenario: Auto feature skips approval
Given a feature at automation level `auto`
When it creates a plan
Then the plan moves `proposed → auto → approved → applying` without an owner action

#### Scenario: Suggest feature waits for approval
Given a feature at automation level `suggest`
When it creates a plan
Then the plan stays in `awaiting_approval` until the owner approves or rejects it

#### Scenario: Observe feature never acts
Given a feature at automation level `observe`
When it produces a finding
Then no plan reaches `approved`

#### Scenario: Illegal transition rejected
Given a plan in `awaiting_approval`
When any actor requests the transition to `applying`
Then the transition is rejected and the plan stays in `awaiting_approval`

### Requirement: Actors and Approval Rights
Every transition MUST record its actor (`scheduler`, `governor`, `owner` or `mcp:<client>`); only the `owner` actor SHALL move a plan from `awaiting_approval` to `approved`, and an `mcp:*` actor MUST NOT trigger the `approved` transition on any plan.

#### Scenario: MCP actor creates a proposal
Given the actor `mcp:claude-code`
When it proposes a cleanup
Then a plan is stored in `awaiting_approval` with the proposer recorded as `mcp:claude-code`

#### Scenario: MCP actor cannot approve
Given a plan in `awaiting_approval`
When the actor `mcp:claude-code` requests the `approved` transition
Then the request is rejected, the plan stays in `awaiting_approval`
And the rejected attempt is written to `action_log`

#### Scenario: Owner approves
Given a plan in `awaiting_approval`
When the `owner` actor approves it
Then the plan moves to `approved` and `action_log` records both the proposer and the approver

### Requirement: Partial Approval
The owner MUST be able to approve all, some or none of a plan's items, and only approved items SHALL be applied.

#### Scenario: Subset approved
Given a plan with items 1 to 5 in `awaiting_approval`
When the owner approves items 2 and 4 only
Then only items 2 and 4 are applied
And items 1, 3 and 5 are recorded as not approved

### Requirement: Verification and Auto-Undo
After apply the framework MUST run verify; when the targeted metric got worse it SHALL alert the owner, and SHALL undo the action automatically only when the action is reversible and the feature's policy allows auto-undo, moving the plan to `rolled_back`.

#### Scenario: Worse result rolled back by policy
Given a reversible applied action whose policy allows auto-undo
When verify reports the targeted metric worse than before
Then undo runs and the plan moves to `rolled_back`

#### Scenario: Worse result without auto-undo
Given a reversible applied action whose policy does not allow auto-undo
When verify reports the targeted metric worse than before
Then the plan stays `applied`, an alert is raised and the undo is offered to the owner

#### Scenario: Better result verified
Given an applied action
When verify reports the targeted metric equal or better
Then the plan moves to `verified`

### Requirement: Append-Only Audit Log
Every plan transition and every action outcome MUST be written to `action_log` with time, plan id, item ids, actor, transition, reason and resulting metrics, and the database MUST reject any UPDATE or DELETE on `action_log` through a trigger.

#### Scenario: UPDATE rejected
Given an existing `action_log` row
When any connection executes an UPDATE on that row
Then the statement fails with an error from the trigger and the row is unchanged

#### Scenario: DELETE rejected
Given an existing `action_log` row
When any connection executes a DELETE on that row
Then the statement fails with an error from the trigger and the row remains

### Requirement: Undo Data Storage
The undo data returned by apply MUST be stored in `undo_blob` linked to its `action_log` entry before the plan moves to `applied`, so that every reversible change can be undone after a restart.

#### Scenario: Undo after restart
Given an applied reversible action whose undo data was stored
When the service restarts and the owner requests undo
Then the action is reversed using the stored undo data

### Requirement: Quarantine Records
Every item moved to quarantine MUST be recorded in `quarantine` with its run id, original path, quarantine location, size and an expiry 7 days after the move, so that restore can return each item to its original path and a purge removes it only after expiry.

#### Scenario: Quarantined item recorded
Given an `ask`-tier item moved into `<volume>\.myclean-quarantine\<run-id>\`
When the move completes
Then a `quarantine` row holds the run id, original path, new location, size and an expiry 7 days later

#### Scenario: Not purged before expiry
Given a quarantined item moved 6 days ago
When the purge job runs
Then the item is kept

#### Scenario: Restore returns item to original path
Given a quarantined item that has not expired
When the owner restores its run
Then the item is moved back to its original path and the restore is written to `action_log`
