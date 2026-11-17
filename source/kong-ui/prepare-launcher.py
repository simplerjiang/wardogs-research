import re
from pathlib import Path

root = Path(__file__).parent
html = (root / 'original/launcher.html').read_text(encoding='utf-8')
html = html.replace('暴龙王助手', 'Kong助手')
html = html.replace('        if (!s.notesSeen && !s.updatePhase && !s.securityMask) showNotes();\n', '')
html = html.replace("waiting:'等待授权'", "waiting:'准备中'").replace("waiting:'Waiting for authorization'", "waiting:'Preparing'")
html = html.replace("byId('detail').textContent=translateNative(s.text)", "byId('detail').textContent=s.stage===0&&!s.failed?'':translateNative(s.text)")
html = html.replace("    const text = String(value || '').trim();", r"""    const text = String(value || '')
        .split(/(?:解决[:：]\s*)?(?:请)?把桌面\s*FATEGEAR(?:诊断|状态|追踪)\.txt/)[0]
        .replace(/\s*Send the FATEGEAR diagnostic files from your Desktop to support(?: if needed)?\./gi, '')
        .trim();""", 1)
html = re.sub(r"    if \(text\.includes\('解决：把桌面 FATEGEAR诊断\.txt'\)\) \{\n.*?\n    \}\n", '', html, count=1, flags=re.S)
html = html.replace("return 'Startup failed. Send the FATEGEAR diagnostic files from your Desktop to support.';", "return 'Startup failed.';")
html = re.sub(r'<div class="steps">.*?</div>', '', html, count=1, flags=re.S)
html = html.replace(' aria-describedby="updateNotes"', '', 1)
start = html.index("const canvas=byId('lightning')")
end = html.index('render(0);', start) + len('render(0);')
html = html[:start] + html[end:]
logo = (root / 'assets/squares-four-fill.svg').read_text(encoding='utf-8').strip()
logo = logo.replace('<svg ', '<svg class="fg-logo" role="img" aria-label="Kong助手" ', 1)
html = re.sub(r'<svg class="fg-logo".*?</svg>', lambda _: logo, html, count=1, flags=re.S)
html = html.replace('<span>Kong助手 / STORM</span>', '<span class="kong-contact">QQ：1013171256</span>')
html = re.sub(r'(<footer>.*?</footer>)</main>', r'</main>\1', html, count=1, flags=re.S)
html = html.replace('<div class="profile-actions">', '<button class="kong-details" id="kongDetails" type="button" aria-expanded="false">展开详情</button><div class="profile-actions">', 1)
css = (root / 'launcher.css').read_text(encoding='utf-8')
html = html.replace('</head>', '<style id="kong-slate">' + css + '</style></head>', 1)
script = '''<script>
document.getElementById('kongDetails').onclick=function(){
 const open=document.body.classList.toggle('show-details');
 this.setAttribute('aria-expanded',String(open));
 this.textContent=open?'收起详情':'展开详情';
};
</script>'''
head, tail = html.rsplit('</body>', 1)
html = head + script + '</body>' + tail
html = re.sub(r' +(?=</html>\s*$)', '', html)
(root / 'launcher.html').write_bytes(html.encode('utf-8'))
print('launcher UTF-16LE bytes:', len(html.encode('utf-16le')))
