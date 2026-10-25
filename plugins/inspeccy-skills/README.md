# Inspeccy Skills

Shared skills: `inspeccy-debug`, `inspeccy-z80-layout`, `inspeccy-next-graphics`.
The same files work in Codex and Claude Code. The two plugin manifests provide
client packaging metadata; no MCP service or model-dependent executor is needed.

Install/build Inspeccy separately and expose it through project configuration
or PATH. Assembly analysis runs in the CLI. Python 3.10+ is needed for the
installer and optional adapters in `tools` (installed at `share/inspeccy/tools`).
Tools and project files must be accessible in the agent's environment. Capability queries detect older executables.

For standalone personal skills, run from Inspeccy's tools directory:

```text
python install_agent_skills.py --client both
```

This copies the three skills to `~/.agents/skills` and `~/.claude/skills` without
changing global settings, hooks or marketplaces. Existing different skills are
preserved unless `--update` is supplied. Updates are limited to files recorded
by a previous installation and reject local edits. Use `--destination <directory>`
with a single client for an explicit skill directory. Start a new client session
after installation. Maintain source here and rerun the installer to update.

Alternatively, load this directory as a plugin through the client's plugin
mechanism. A Claude Code development session can use:

```text
claude --plugin-dir /path/to/inspeccy-skills
```

The plugin skills are namespaced, for example `/inspeccy-skills:inspeccy-debug`.
Codex uses `.codex-plugin/plugin.json`; Claude uses `.claude-plugin/plugin.json`.
Standalone skill installation avoids requiring a local marketplace.

The Codex manifest declares local execution because these skills run native
programs and read local build artifacts. Keep Inspeccy and the program under
test available in the client's execution environment.

The personal Codex directory follows the [local skill documentation](https://learn.chatgpt.com/docs/build-skills).
To update an older installation elsewhere, pass that directory explicitly
with `--client codex --destination PATH --update`. The installer never moves
or removes an existing installation automatically. Avoid installing both the
plugin and personal copies unless you want duplicate skill entries.

Manifest references: [Codex packaging](https://developers.openai.com/plugins/build/plugins),
[local execution](https://developers.openai.com/plugins/deploy/desktop-only),
and [Claude plugin paths](https://code.claude.com/docs/en/plugins-reference).
