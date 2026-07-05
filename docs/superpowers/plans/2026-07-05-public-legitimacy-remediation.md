# Public Legitimacy Remediation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Remove misleading release, affiliation, and third-party dependency signals from the public repository.

**Architecture:** Documentation-only remediation. Public docs will clearly distinguish Halcyon code from IBM-owned Db2 assets, avoid linking to non-existent releases, and add basic security/third-party notices.

**Tech Stack:** Markdown, CMake documentation, GitHub repository metadata.

---

### Task 1: Public README and Changelog Corrections

**Files:**
- Modify: `README.md`
- Modify: `CHANGELOG.md`

- [x] **Step 1: Replace false release-tag consumption guidance**

Change the FetchContent snippet in `README.md` from `release-tag pin` to `GIT_TAG main`, with a short note that consumers should pin a reviewed commit or real release tag.

- [x] **Step 2: Remove dead changelog release links**

Change `CHANGELOG.md` so the `1.0.0` section is plain text rather than a link to a non-existent GitHub Release. Keep the content but remove invalid compare/release footnotes.

- [x] **Step 3: Add non-affiliation and driver-provenance language**

Add a short public notice that Halcyon is independent and not affiliated with, endorsed by, or sponsored by IBM. Replace "vendored" language with "user-supplied" language.

### Task 2: Third-Party and Security Notices

**Files:**
- Create: `THIRD_PARTY_NOTICES.md`
- Create: `SECURITY.md`
- Modify: `README.md`

- [x] **Step 1: Create third-party notice**

Document IBM Db2 CLI driver and Db2 Community container provenance, license responsibility, and non-redistribution.

- [x] **Step 2: Create security policy**

Document supported versions and responsible disclosure contact through GitHub Security Advisories or private maintainer contact.

- [x] **Step 3: Link notices from README**

Add links to `THIRD_PARTY_NOTICES.md` and `SECURITY.md` in the license/trust area of the README.

### Task 3: Docker License Acceptance Clarity

**Files:**
- Modify: `README.md`
- Modify: `docker/README.md`

- [x] **Step 1: Clarify Docker license acceptance**

Near the Docker integration commands, state that starting the Db2 container sets `LICENSE=accept` and requires the runner to accept IBM's terms.

### Task 4: Verification, Merge, Cleanup, Push

**Files:**
- Public documentation files only.

- [x] **Step 1: Run focused text checks**

Run focused `rg` checks for dead release links, old dependency wording, affiliation language, `SECURITY`, and `THIRD_PARTY_NOTICES`.

- [x] **Step 2: Build docs if local docs tooling is available**

Run `docs/build.sh` if the existing environment has the dependencies. If not, report the blocker.

- [x] **Step 3: Commit, merge to main, remove worktree, push**

Commit the remediation branch, merge it to `main`, delete the linked worktree, and push `main`.
