# Approvals Specification

## ADDED Requirements

### Requirement: Approval Inbox
The tray SHALL show every plan in state `awaiting_approval` with its items, total bytes, risk, tier and the actor that proposed it, and SHALL let the owner approve all, some or none of its items.

#### Scenario: Pending plan shown
Given a plan with 12 shader-cache items totalling 3.1 GB, tier `ask`, proposed by `scheduler`
When the owner opens the approval inbox
Then the plan is listed with 12 items, 3.1 GB, its risk, tier `ask` and proposer `scheduler`

#### Scenario: Approve some
Given a pending plan with items A, B and C
When the owner approves only A and C
Then only A and C are executed
And B is recorded as rejected and never executed

#### Scenario: Approve none
Given a pending plan
When the owner rejects all of its items
Then no item is executed and the plan is closed as rejected

### Requirement: Approval Only From the Tray
The `approved` transition SHALL be accepted only from the tray process in the owner's interactive session, checked by the client process image and session; the CLI, the MCP server and any other pipe client MUST NOT approve a plan.

#### Scenario: CLI cannot approve
Given a plan in state `awaiting_approval`
When `myclean.exe` sends `plan.approve` for it
Then the request is refused
And the plan stays in `awaiting_approval`

#### Scenario: MCP cannot approve
Given an AI client connected to `myclean mcp` created a cleanup plan
When the client tries to approve or execute the plan by any tool call
Then no tool approves or executes it
And the audit log shows no `approved` transition by an `mcp:*` actor

### Requirement: MCP Proposal Tools
The MCP server SHALL expose `propose_cleanup` and `propose_optimization`, each creating a plan in state `awaiting_approval` with actor `mcp:<client>` and raising a tray notification, and SHALL accept only item identifiers produced by MyClean's own scans, never raw paths or commands.

#### Scenario: Proposal waits for the tray
Given an AI client calls `propose_cleanup` with three item ids from a `find_old_downloads` result
When the call returns
Then a plan in state `awaiting_approval` with those three items exists
And a tray notification is shown
And no file has been touched

#### Scenario: Raw path rejected
Given an AI client calls `propose_cleanup` with the raw path `C:\Users\<owner>\Documents\report.docx`
When the call is processed
Then no plan is created and the tool returns an error

### Requirement: MCP Read Tools for Storage
The MCP server SHALL expose the read-only tools `storage_top`, `find_old_downloads`, `find_duplicates` and `find_used_installers`, returning the same data as the Space view, and MUST NOT change any file or setting when they are called.

#### Scenario: Prompt 3 over MCP
Given the owner pastes prompt 3 into Claude Code with `myclean mcp` registered
When the assistant calls the four storage read tools
Then it receives the top-20 items, old downloads, duplicates and used installers with sizes
And the audit log shows no action executed

### Requirement: Plan Audit Trail
Every plan state transition SHALL be written to the append-only audit log with its actor (`scheduler`, `governor`, `owner` or `mcp:<client>`), and the system MUST NOT perform any file operation that does not belong to a plan.

#### Scenario: Full trail
Given an MCP-proposed plan was approved in the tray and executed
When the audit log is read
Then it shows `proposed` by `mcp:<client>`, `awaiting_approval`, `approved` by `owner`, `applying` and `applied`

#### Scenario: Audit rows are immutable
Given an audit log row exists
When any component tries to update or delete it
Then the store refuses the change
