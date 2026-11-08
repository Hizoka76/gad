"""Catalogue des éditions, guides et captures ; aucune dépendance de rendu à l’import."""
from dataclasses import dataclass


@dataclass(frozen=True)
class Guide:
    source: str
    slug: str
    language: str
    audience: str
    title: str


LABELS = {
    'fr': {'locale':'fr_FR','language':'Français','quick':'Prise en main en quelques minutes','user':'Mode d’emploi illustré','developer':'Architecture et fonctionnement interne','icon':'Icône Gad','project':'Projet Gad','meta':'Documentation du projet · Lecture hors ligne<br>Version 0.2.0 · Novembre 2026','troubleshooting':'problemes-frequents','titles':('Démarrage rapide','Guide utilisateur','Guide développeur')},
    'en-US': {'locale':'en_US','language':'English (US)','quick':'Get started in a few minutes','user':'Illustrated User Guide','developer':'Architecture and Internal Behavior','icon':'Gad icon','project':'Gad project','meta':'Project documentation · Offline edition<br>Version 0.2.0 · November 2026','troubleshooting':'troubleshooting','titles':('Quick Start','User Guide','Developer Guide')},
    'es-ES': {'locale':'es_ES','language':'Español','quick':'Primeros pasos en pocos minutos','user':'Guía de usuario ilustrada','developer':'Arquitectura y funcionamiento interno','icon':'Icono de Gad','project':'Proyecto Gad','meta':'Documentación del proyecto · Edición sin conexión<br>Versión 0.2.0 · Noviembre de 2026','troubleshooting':'troubleshooting','titles':('Inicio rápido','Guía de usuario','Guía de desarrollo')},
    'pt-PT': {'locale':'pt_PT','language':'Português','quick':'Começar em poucos minutos','user':'Guia do utilizador ilustrado','developer':'Arquitetura e funcionamento interno','icon':'Ícone do Gad','project':'Projeto Gad','meta':'Documentação do projeto · Edição offline<br>Versão 0.2.0 · Novembro de 2026','troubleshooting':'troubleshooting','titles':('Início rápido','Guia do utilizador','Guia de desenvolvimento')},
    'de-DE': {'locale':'de_DE','language':'Deutsch','quick':'In wenigen Minuten loslegen','user':'Bebildertes Benutzerhandbuch','developer':'Architektur und interne Abläufe','icon':'Gad-Symbol','project':'Gad-Projekt','meta':'Projektdokumentation · Offline-Ausgabe<br>Version 0.2.0 · November 2026','troubleshooting':'troubleshooting','titles':('Schnellstart','Benutzerhandbuch','Entwicklerhandbuch')},
    'ja-JP': {'locale':'ja_JP','language':'日本語','quick':'数分で使い始める','user':'図解ユーザーガイド','developer':'アーキテクチャと内部動作','icon':'Gad のアイコン','project':'Gad プロジェクト','meta':'プロジェクトドキュメント · オフライン版<br>バージョン 0.2.0 · 2026 年 11 月','troubleshooting':'troubleshooting','titles':('クイックスタート','ユーザーガイド','開発者ガイド')},
}
LANGUAGES = tuple(LABELS)
AUDIENCES = ('quick','user','developer')
CAPTURE_COUNT = 22


def filename(language, audience):
    if language == 'fr':
        return {'quick':'DEMARRAGE_RAPIDE.md','user':'GUIDE_UTILISATEUR.md','developer':'GUIDE_DEVELOPPEUR.md'}[audience]
    return {'quick':'QUICK_START','user':'USER_GUIDE','developer':'DEVELOPER_GUIDE'}[audience]+'.'+language+'.md'


def portal(language):
    return 'README.md' if language == 'fr' else 'README.'+language+'.md'


GUIDES = tuple(Guide(filename(lang, audience),
                     ({'quick':'demarrage-rapide','user':'guide-utilisateur','developer':'guide-developpeur'}[audience] if lang=='fr' else {'quick':'quick-start','user':'user-guide','developer':'developer-guide'}[audience]+'.'+lang),
                     lang,audience,LABELS[lang]['titles'][AUDIENCES.index(audience)])
               for lang in LANGUAGES for audience in AUDIENCES)


def peers(guide):
    return tuple(item for item in GUIDES if item.audience==guide.audience and item.language!=guide.language)


def peer(guide):
    """Premier guide de même public dans une autre langue ; conservé pour les appelants existants."""
    return peers(guide)[0]


def runtime_catalog():
    """Catalogue utilisé par À propos ; les noms et titres viennent des mêmes métadonnées que l’export."""
    return {"format":"gad-documentation","version":1,"guides":[
        {"locale":LABELS[guide.language]["locale"],"kind":guide.audience,"title":guide.title,
         "html":"html/"+guide.slug+".html","pdf":"pdf/"+guide.slug+".pdf"} for guide in GUIDES]}
