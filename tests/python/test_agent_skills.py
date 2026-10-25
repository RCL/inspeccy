"""Shared skill packaging and installer behaviour, without calling a model."""
import importlib.util
import json
from pathlib import Path
import re
import shutil
import tempfile

SOURCE = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('installer', SOURCE/'tools/install_agent_skills.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


def main():
    package = SOURCE/'plugins/inspeccy-skills'
    codex = json.loads((package/'.codex-plugin/plugin.json').read_text())
    claude = json.loads((package/'.claude-plugin/plugin.json').read_text())
    assert codex['requires_local_executor'] is True
    assert codex['name'] == claude['name'] and codex['skills'] == claude['skills'] == './skills/'
    for skill in (package/'skills').iterdir():
        text = (skill/'SKILL.md').read_text()
        assert text.startswith('---\nname: '+skill.name+'\n') and '\ndescription: ' in text
        for reference in re.findall(r'\]\((references/[^)]+)\)', text):
            assert (skill/reference).is_file(), reference
    with tempfile.TemporaryDirectory(prefix='inspeccy-skills-') as temp:
        root = Path(temp)
        roots = [root/'codex', root/'claude']
        installed = installer.install(package, roots)
        assert len(installed) == 6
        assert installer.install(package, roots) == installed
        for relative in ('SKILL.md', 'references/experiments.md'):
            assert (roots[0]/'inspeccy-debug'/relative).read_bytes() == (roots[1]/'inspeccy-debug'/relative).read_bytes()
        # A changed owned package updates both clients from one source.
        revised = root/'package'; shutil.copytree(package, revised)
        changed = revised/'skills/inspeccy-debug/references/experiments.md'
        changed.write_text(changed.read_text()+'\nRevision.\n')
        try:
            installer.install(revised, roots)
            raise AssertionError('unrequested overwrite')
        except ValueError:
            pass
        installer.install(revised, roots, update=True)
        # One locally edited copy prevents all writes, including the other client.
        local = roots[1]/'inspeccy-debug/SKILL.md'; local.write_text('local edit')
        first_before = (roots[0]/'inspeccy-debug/references/experiments.md').read_bytes()
        changed.write_text(changed.read_text()+'Another revision.\n')
        try:
            installer.install(revised, roots, update=True)
            raise AssertionError('overwrote a local edit')
        except ValueError:
            pass
        assert local.read_text() == 'local edit'
        assert (roots[0]/'inspeccy-debug/references/experiments.md').read_bytes() == first_before
    print('PASS: shared Codex/Claude skill package, reference links and safe install/update')


if __name__ == '__main__':
    main()
