#!/usr/bin/env python3
"""Régénère les index localisés à partir du catalogue des guides."""
from pathlib import Path
from doc_catalog import GUIDES,LANGUAGES,LABELS,portal,CAPTURE_COUNT
ROOT=Path(__file__).resolve().parent.parent
TEXT={'fr': ('Documentation',
        'Choisir un guide',
        'Document',
        'Source',
        'HTML',
        '',
        'Autres langues',
        'Régénération',
        'Chaque langue propose un démarrage rapide, un guide utilisateur et un guide développeur. Les guides utilisateur contiennent vingt-deux captures des vrais widgets en '
        'thème clair, dans leur langue.',
        'Les 18 guides sont fournis en Markdown et HTML autonome.',
        'Les captures utilisent un moteur de démonstration, des fichiers temporaires et des sessions privées. Aucun transfert ni presse-papiers du bureau n’est piloté.',
        'L’export nécessite Python avec Markdown. Les captures nécessitent aussi les dépendances natives de Gad.',
        'Tests et scénarios'),
 'en-US': ('Documentation',
           'Choose a guide',
           'Document',
           'Source',
           'HTML',
           '',
           'Other languages',
           'Regeneration',
           'Each language has a Quick Start, User Guide, and Developer Guide. User guides include twenty-two screenshots of real widgets in the light theme, in their own '
           'language.',
           'All 18 guides are available as Markdown and standalone HTML.',
           'Captures use a demo engine, temporary files, and isolated sessions. They never control real desktop transfers or the clipboard.',
           'Export requires Python with Markdown. Captures also require Gad’s native build dependencies.',
           'Tests and scenarios'),
 'es-ES': ('Documentación',
           'Elegir una guía',
           'Documento',
           'Fuente',
           'HTML',
           '',
           'Otros idiomas',
           'Regeneración',
           'Cada idioma tiene una guía de inicio rápido, una guía de usuario y una guía de desarrollo. Las guías de usuario incluyen veintidós capturas de los widgets reales con '
           'el tema claro y en su propio idioma.',
           'Las 18 guías están disponibles en Markdown y HTML autónomo.',
           'Las capturas utilizan un motor de demostración, archivos temporales y sesiones aisladas. No controlan las transferencias reales ni el portapapeles del escritorio.',
           'La exportación requiere Python con Markdown. Las capturas también requieren las dependencias nativas de Gad.',
           'Pruebas y escenarios'),
 'pt-PT': ('Documentação',
           'Escolher um guia',
           'Documento',
           'Fonte',
           'HTML',
           '',
           'Outros idiomas',
           'Regeneração',
           'Cada idioma tem um guia de início rápido, um guia do utilizador e um guia de desenvolvimento. Os guias do utilizador incluem vinte e duas capturas dos widgets reais '
           'com o tema claro e no respetivo idioma.',
           'Os 18 guias estão disponíveis em Markdown e HTML autónomo.',
           'As capturas utilizam um motor de demonstração, ficheiros temporários e sessões isoladas. Não controlam transferências reais nem a área de transferência do ambiente de '
           'trabalho.',
           'A exportação requer Python com Markdown. As capturas também requerem as dependências nativas do Gad.',
           'Testes e cenários'),
 'de-DE': ('Dokumentation',
           'Ein Handbuch auswählen',
           'Dokument',
           'Quelle',
           'HTML',
           '',
           'Weitere Sprachen',
           'Neu erzeugen',
           'Für jede Sprache gibt es einen Schnellstart, ein Benutzerhandbuch und ein Entwicklerhandbuch. Die Benutzerhandbücher enthalten zweiundzwanzig Aufnahmen der echten '
           'Oberfläche im hellen Design und in ihrer jeweiligen Sprache.',
           'Alle 18 Handbücher sind als Markdown und eigenständiges HTML verfügbar.',
           'Die Aufnahmen verwenden eine Demo-Engine, temporäre Dateien und isolierte Sitzungen. Echte Übertragungen und die Zwischenablage des Desktops werden nicht gesteuert.',
           'Für den Export werden Python und Markdown benötigt. Die Aufnahmen benötigen zusätzlich die nativen Build-Abhängigkeiten von Gad.',
           'Tests und Szenarien'),
 'ja-JP': ('ドキュメント',
           'ガイドを選ぶ',
           'ドキュメント',
           'ソース',
           'HTML',
           '',
           '他の言語',
           '再生成',
           '各言語にクイックスタート、ユーザーガイド、開発者ガイドがあります。ユーザーガイドには、その言語の実際の画面をライトテーマで撮影した22枚の画像が含まれています。',
           '18冊のガイドをMarkdownとHTMLで提供します。',
           '撮影にはデモ用エンジン、一時ファイル、隔離されたセッションを使用します。実際の転送やデスクトップのクリップボードは操作しません。',
           '書き出しにはMarkdownを利用できるPython環境が必要です。画面撮影にはGadのネイティブビルド依存関係も必要です。',
           'テストとシナリオ')}


def main():
    for lang in LANGUAGES:
        t=TEXT[lang];lines=['# Gad — '+t[0],'','Version 0.3.0 · '+LABELS[lang]['language'],'','## '+t[1],'',t[8],'','| '+' | '.join(t[2:5])+' |','| --- | --- | --- |']
        for guide in GUIDES:
            if guide.language==lang:lines.append('| '+guide.title+' | [Markdown]('+guide.source+') | [HTML](html/'+guide.slug+'.html) |')
        testing='TESTING.md' if lang=='fr' else 'TESTING.'+lang+'.md'
        lines+=['',t[9],'','['+t[12]+'](../'+testing+')','','## '+t[6],'']
        lines+=[' · '.join('['+LABELS[other]['language']+']('+portal(other)+')' for other in LANGUAGES if other!=lang),'','## '+t[7]+' {#regeneration}','',t[10],'',t[11],'','```sh','make docs-captures','make docs','make docs-check','```','',f'{CAPTURE_COUNT*len(LANGUAGES)} captures · 18 HTML','']
        (ROOT/'docs'/portal(lang)).write_text('\n'.join(lines))


if __name__=='__main__':main()
