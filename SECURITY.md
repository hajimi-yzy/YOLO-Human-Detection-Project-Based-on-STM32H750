# Security and public-repository hygiene

Please report vulnerabilities through a private GitHub security advisory. Do not place live server addresses, credentials, tokens, device locations, screenshots, logs, captures, or working exploits in a public issue.

## Before publishing or deploying

- Keep real IP addresses, domains, stream URLs, AP passwords, administrator credentials and tokens outside Git.
- Treat `192.0.2.10` as a documentation-only TEST-NET address and every `CHANGE_ME_*` value as an invalid deployment placeholder.
- Do not commit effective `sdkconfig`, `.env`, build/staging directories, logs, packet captures or firmware binaries containing deployment values.
- The demo login placeholders in this repository are not production authentication. Add proper authentication and use HTTPS/WSS before Internet exposure.
- Restrict `8765/tcp`, `9091/udp`, `9092/tcp` and `9093/udp` with a firewall, security group, VPN or private network as appropriate.
- Keep the STM32 500 ms local control watchdog and a physical emergency-stop path; cloud acknowledgements are not proof of actuator execution.

If a real value was committed previously, replacing it in a new commit does not remove it from Git history. Rotate the exposed credential/address where possible, then coordinate a deliberate history rewrite with all repository users if full removal is required.
