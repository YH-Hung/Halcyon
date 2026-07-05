# Security Policy

## Supported Versions

Halcyon is currently distributed as source from the public repository. Security
fixes are made on `main` until release branches or published tags exist.

## Reporting a Vulnerability

Please do not open a public issue for a suspected vulnerability.

Use GitHub Security Advisories for this repository when available. If advisories
are not available to you, contact the maintainer privately through the GitHub
account that owns this repository and include:

- Affected commit, branch, or version.
- A short description of the issue and impact.
- Reproduction steps or a proof of concept, if you can share one safely.
- Whether the issue depends on IBM Db2, the IBM Db2 CLI driver, or deployment
  configuration outside Halcyon.

The maintainer will acknowledge valid reports as soon as practical, investigate,
and coordinate public disclosure after a fix or mitigation is available.

## Third-Party Components

Halcyon depends on a user-supplied IBM Db2 CLI driver at build/link time and may
be tested against IBM Db2 container images. Vulnerabilities in IBM-provided
components should also be reported through IBM's security reporting process.
