# Trust Specification

## ADDED Requirements

### Requirement: Signature Check
The system SHALL determine for every process image whether it is validly signed using `WinVerifyTrust`, including catalog signatures, and SHALL record the signer name of a valid signature.

#### Scenario: Catalog-signed system file
Given `svchost.exe` in `C:\Windows\System32` carries no embedded signature but is in a Windows catalog
When its signature is checked
Then it is reported as signed with signer Microsoft Windows

#### Scenario: Unsigned image
Given an image with no embedded signature and no catalog entry
When its signature is checked
Then it is reported as unsigned with no signer

#### Scenario: Tampered image
Given a signed image whose bytes were modified after signing
When its signature is checked
Then it is reported as invalid signature, not as signed

### Requirement: Path Class
The system SHALL classify every process image path as user-writable when it lies under a location a standard user can write (such as `%TEMP%`, `AppData` or `Downloads`) and as protected otherwise.

#### Scenario: Image in AppData
Given a process runs from `C:\Users\<owner>\AppData\Local\Foo\foo.exe`
When its path is classified
Then its path class is user-writable

#### Scenario: Image in Program Files
Given a process runs from `C:\Program Files\Foo\foo.exe`
When its path is classified
Then its path class is protected

### Requirement: Knowledge Base Data
The system SHALL load the process knowledge base from `data/kb/*.json`, where each entry matches on image name, signer and path pattern and holds a plain-language description, a category, whether it is needed at startup and whether it is safe to remove; a file that fails schema validation MUST be rejected whole.

#### Scenario: Valid knowledge base
Given `data/kb/processes.json` is valid
When the service starts
Then every entry is available for matching

#### Scenario: Invalid entry
Given a knowledge-base file in which one entry lacks its description
When the file is loaded
Then the whole file is rejected with an error naming the file and entry, and none of its entries are used

### Requirement: Knowledge Base v1 Coverage
Knowledge base v1 SHALL describe the Windows core processes and the resident applications of the reference machine, including the known clipboard and input suspects (PowerToys Keyboard Manager, AnyDesk, clipboard history service, Armoury Crate, Discord).

#### Scenario: Known suspect
Given `PowerToys.KeyboardManagerEngine.exe` signed by Microsoft is running from its install folder
When it is matched against the knowledge base
Then it matches an entry with a plain description and category

### Requirement: Trust Signal
The system SHALL attach a trust signal to every process with its signature result, signer, path class and knowledge-base match.

#### Scenario: Trust signal fields
Given any running process
When its trust signal is requested
Then the answer contains signature result, signer or none, path class, and the matching knowledge-base entry or none

### Requirement: Unrecognized Flag
The system SHALL flag a process as unrecognized if and only if it has no knowledge-base match AND at least one of: unsigned or invalid signature, a signer the knowledge base does not list, or a user-writable path; the flag SHALL state which of these reasons apply.

#### Scenario: Unknown, unsigned process
Given a process with no knowledge-base match and no signature
When its trust signal is computed
Then it is flagged unrecognized with the reasons not in knowledge base and unsigned

#### Scenario: Unknown process from a user-writable path
Given a process with no knowledge-base match, signed by a signer the knowledge base lists, running from `%TEMP%`
When its trust signal is computed
Then it is flagged unrecognized with the reasons not in knowledge base and user-writable path

#### Scenario: Known process that is unsigned
Given a process that matches a knowledge-base entry but is unsigned
When its trust signal is computed
Then it is not flagged unrecognized

#### Scenario: Unknown but trusted
Given a process with no knowledge-base match, signed by a signer the knowledge base lists, running from Program Files
When its trust signal is computed
Then it is not flagged unrecognized

### Requirement: No Malware Verdict
The trust signal MUST NOT label any process as dangerous or malicious; it SHALL only report the facts that make a process unrecognized.

#### Scenario: Unrecognized is not a verdict
Given a process flagged unrecognized
When it is shown in the UI, CLI or MCP output
Then it is described as unrecognized with its reasons and never as dangerous or malicious
