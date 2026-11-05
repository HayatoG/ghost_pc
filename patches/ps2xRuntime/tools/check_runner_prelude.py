#!/usr/bin/env python
"""Confere que a visao leve (ps2_runner_prelude.h) nao divergiu da classe completa (ps2_runtime.h).

Cada declaracao de metodo/enum/typedef do corpo de `class PS2Runtime` do prelude precisa aparecer,
com a mesma assinatura (espacos normalizados), no corpo de `class PS2Runtime` de ps2_runtime.h.
Tambem lista os metodos chamados pelo codigo gerado (runtime->X) que nao estao no prelude.

Uso: python -I check_runner_prelude.py <ps2xRuntime/include> [<ps2xRuntime/src/runner>]
Saida 0 = ok; 1 = divergencia. Log em stdout (redirecione para logs/ se quiser guardar).
"""
import re
import sys
from pathlib import Path


def class_body(text: str) -> str:
    i = text.index('class PS2Runtime')
    j = text.index('{', i)
    depth = 0
    for k in range(j, len(text)):
        depth += (text[k] == '{') - (text[k] == '}')
        if depth == 0:
            return text[j + 1:k]
    raise ValueError('classe nao fechada')


def statements(body: str):
    body = re.sub(r'//[^\n]*', '', body)
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    out, cur, depth = [], '', 0
    for ch in body:
        cur += ch
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                out.append(cur)
                cur = ''
        elif ch == ';' and depth == 0:
            out.append(cur)
            cur = ''
    def norm(s):
        s = re.sub(r'(public|private|protected)\s*:', '', s)
        return re.sub(r'\s+', ' ', s).strip().replace('( ', '(').replace(' )', ')')
    return [norm(s) for s in out if norm(s)]


def main() -> int:
    inc = Path(sys.argv[1])
    full = statements(class_body((inc / 'ps2_runtime.h').read_text(encoding='utf-8')))
    light = statements(class_body((inc / 'ps2_runner_prelude.h').read_text(encoding='utf-8')))
    fullset = set(full)
    bad = [s for s in light if s not in fullset]
    for s in bad:
        print('[ERRO] no prelude mas nao (identico) em ps2_runtime.h:', s)
    if len(sys.argv) > 2:
        used = set()
        for p in Path(sys.argv[2]).glob('*.cpp'):
            used.update(re.findall(r'\bruntime\s*->\s*(\w+)', p.read_text(encoding='utf-8', errors='replace')))
        macros = re.sub(r'//[^\n]*', '', (inc / 'ps2_runtime_macros.h').read_text(encoding='utf-8'))
        used.update(re.findall(r'\bruntime\s*->\s*(\w+)', macros))
        declared = ' '.join(light)
        for name in sorted(used):
            if not re.search(r'\b%s\s*\(' % re.escape(name), declared):
                print('[ERRO] codigo gerado chama runtime->%s, ausente do prelude' % name)
                bad.append(name)
    print('prelude: %d declaracoes, %d divergencias' % (len(light), len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
