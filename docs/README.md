# Gad — Documentation

Version 0.3.0 · Français

## Choisir un guide

Chaque langue propose un démarrage rapide, un guide utilisateur et un guide développeur. Les guides utilisateur contiennent vingt-deux captures des vrais widgets en thème clair, dans leur langue.

| Document | Source | HTML |
| --- | --- | --- |
| Démarrage rapide | [Markdown](DEMARRAGE_RAPIDE.md) | [HTML](html/demarrage-rapide.html) |
| Guide utilisateur | [Markdown](GUIDE_UTILISATEUR.md) | [HTML](html/guide-utilisateur.html) |
| Guide développeur | [Markdown](GUIDE_DEVELOPPEUR.md) | [HTML](html/guide-developpeur.html) |

Les 18 guides sont fournis en Markdown et HTML autonome.

[Tests et scénarios](../TESTING.md)

## Autres langues

[English (US)](README.en-US.md) · [Español](README.es-ES.md) · [Português](README.pt-PT.md) · [Deutsch](README.de-DE.md) · [日本語](README.ja-JP.md)

## Régénération {#regeneration}

Les captures utilisent un moteur de démonstration, des fichiers temporaires et des sessions privées. Aucun transfert ni presse-papiers du bureau n’est piloté.

L’export nécessite Python avec Markdown. Les captures nécessitent aussi les dépendances natives de Gad.

```sh
make docs-captures
make docs
make docs-check
```

138 captures · 18 HTML
