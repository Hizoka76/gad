# Gad — Documentação

Version 0.1.0 · Português

## Escolher um guia

Cada idioma tem um guia de início rápido, um guia do utilizador e um guia de desenvolvimento. Os guias do utilizador incluem catorze capturas dos widgets reais com o tema claro e no respetivo idioma.

| Documento | Fonte | HTML | PDF |
| --- | --- | --- | --- |
| Início rápido | [Markdown](QUICK_START.pt-PT.md) | [HTML](html/quick-start.pt-PT.html) | [PDF](pdf/quick-start.pt-PT.pdf) |
| Guia do utilizador | [Markdown](USER_GUIDE.pt-PT.md) | [HTML](html/user-guide.pt-PT.html) | [PDF](pdf/user-guide.pt-PT.pdf) |
| Guia de desenvolvimento | [Markdown](DEVELOPER_GUIDE.pt-PT.md) | [HTML](html/developer-guide.pt-PT.html) | [PDF](pdf/developer-guide.pt-PT.pdf) |

Os 18 guias estão disponíveis em Markdown, HTML autónomo e PDF. Os PDF em inglês usam US Letter; as outras edições usam A4.

[Testes e cenários](../TESTING.pt-PT.md)

## Outros idiomas

[Français](README.md) · [English (US)](README.en-US.md) · [Español](README.es-ES.md) · [Deutsch](README.de-DE.md) · [日本語](README.ja-JP.md)

## Regeneração {#regeneration}

As capturas utilizam um motor de demonstração, ficheiros temporários e sessões isoladas. Não controlam transferências reais nem a área de transferência do ambiente de trabalho.

A exportação requer Python com Markdown, Graphviz e WeasyPrint. As capturas também requerem as dependências nativas do Gad.

```sh
make docs-captures
make docs
make docs-check
```

84 captures · 18 HTML · 18 PDF
