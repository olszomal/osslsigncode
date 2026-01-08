---
name: Bug report
about: Report a bug
labels: bug
---

<!--
Note: Please search to see if an issue already exists for the bug you encountered.
-->

### Current Behavior
<!--
A concise description of what is happening.
Include error messages or incorrect results.
-->

### Expected Behavior
<!--
A concise description of what you expected to happen instead.
-->

### Steps To Reproduce & Observed Output
<!--
Provide exact, reproducible steps together with full stdout/stderr for each.
-->
1. Signing with osslsigncode
<!--
Full `osslsigncode sign` command and complete stdout/stderr output.
-->

2. Verification with osslsigncode
<!--
Full `osslsigncode verify` command and complete stdout/stderr output.
-->

3. Signing / verification with Windows signtool (if applicable)
<!--
Full signtool command (`signtool verify /pa /v`) and complete stdout/stderr output.
-->

### Environment
- Operating system and version (e.g. Ubuntu 24.04):
- Architecture (x86_64, arm64, etc.):

### Versions
<!--
Specify whether osslsigncode is built from master or a released version.
-->
- osslsigncode built from:
  - [ ] source
  - [ ] distribution package
- `openssl version -a`
- `osslsigncode --version`

### Files
<!--
Attach files if possible, or mention that you will share them privately.
-->
- [ ] unsigned file
- [ ] file signed with osslsigncode
- [ ] file signed with signtool or the other tool (for comparison)
- [ ] certificate chain used for verification (PEM format)

### Configuration / Settings
<!-- Anything that could affect signing or verification:
- Custom OpenSSL configuration
- Engine / provider settings
- Environment variables (OPENSSL_CONF, etc.)
-->

### Anything else
<!--
Links, references, related issues, or additional observations.
-->

<!--
Thanks for the detailed report.
-->
