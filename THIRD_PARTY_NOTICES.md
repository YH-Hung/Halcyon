# Third-Party Notices

Halcyon is an independent open-source project licensed under the BSD 3-Clause
License. It does not redistribute IBM Db2, the IBM Db2 CLI driver, or IBM Db2
container images.

## IBM Db2 and IBM Db2 CLI Driver

Halcyon builds against IBM Db2 CLI headers and links to the IBM Db2 CLI driver
library supplied by the user. The default local lookup path is
`third_party/clidriver`, or callers can set `DB2_CLIDRIVER_ROOT`,
`DB2CLI_INCLUDE_DIR`, or `DB2CLI_LIBRARY`.

The IBM Db2 CLI driver is governed by IBM's own license terms. Users are
responsible for obtaining the driver from IBM or another authorized source,
reviewing its terms, and ensuring their use is permitted.

## IBM Db2 Community Container

The integration-test Docker Compose file uses the IBM Db2 Community image from
IBM Container Registry and sets `LICENSE=accept`. Running that container means
the runner is accepting IBM's applicable container/image license terms.

## Trademarks

IBM and Db2 are trademarks of International Business Machines Corporation.
Halcyon is not affiliated with, endorsed by, or sponsored by IBM.
