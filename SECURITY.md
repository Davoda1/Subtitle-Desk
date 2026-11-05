# Security policy

## Reporting a vulnerability

If **Security → Advisories → Report a vulnerability** is available for this repository, use that private reporting channel. Otherwise, open an issue requesting a private reporting channel without posting the vulnerability details. Do not put exploit files, private media, credentials, or identifying information into a public issue.

Include the affected version and platform, a clear description of the impact, and the smallest reproduction you can safely share. For malformed media or subtitle files, describe the relevant format and provide a non-sensitive sample through the agreed channel. Reports against the latest release are preferred; include older affected versions when known.

## Scope

Relevant reports include unsafe subtitle or project parsing, malicious media handling, unsafe file writes, unintended network access, and defects in dependency or release integrity checks. Subtitle Desk processes local files using native components, so report the application version and package source as well as the operating system.

Speech recognition mistakes, approximate timing, and unsupported media codecs are normally product issues rather than security vulnerabilities. Use the regular issue templates for those reports.

## Updates and dependencies

Security fixes are published through repository commits and releases. Third-party engines, Qt, FFmpeg, and model weights retain their upstream licenses and provenance. The runtime lock records pinned distribution and model checksums; those checks should not be disabled when building or packaging.
