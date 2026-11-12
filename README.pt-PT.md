# Gad {#gad}

[Français](README.md) · [English (US)](README.en-US.md) · [Español](README.es-ES.md) · [Português](README.pt-PT.md) · [Deutsch](README.de-DE.md) · [日本語](README.ja-JP.md)

**Gad, il copie.** — **Gestionnaire Avancé de Duplication** (Gestor avançado de duplicação)

[Francês](README.md) · [Início rápido](docs/QUICK_START.pt-PT.md)

Gad traz cópias e movimentos de ficheiros para uma janela KDE Plasma nativa. O seu mecanismo de transferência está escrito em Rust, com interface C++/Qt 6 Widgets.

- Interfaces em francês, inglês dos EUA, espanhol, português, alemão e japonês com preferência de idioma guardada.
- Uma fila detalhada com árvores de pastas, prioridades e linhas arrastáveis.
- Até duas transferências compatíveis em paralelo, renomeações rápidas e supressão automática de duplicados.
- Controlos de pausa, limites de largura de banda, estimativas por pedido e seguimento de progresso Plasma.
- Conflitos legíveis, verificações de MD5/propriedade/espaço livre e histórico com desfazer.
- Recuperação parcial de ficheiros, espera pela reconexão do dispositivo e novas tentativas limitadas a ficheiros com falha.
- Fontes e destinos locais e remotos através do fusível KIO: KDE Connect, FTP, SFTP e SMB.


**0.4.0** — Escolha destino e opções e adicione os ficheiros no Gad. Sem trabalho ativo, reveja a lista e clique em **Iniciar**. A colagem externa pelo gerenciador de arquivos inicia automaticamente; novos elementos durante uma transferência entram na fila. **Ctrl+C** copia e **Ctrl+X** move. Arrastar e largar prepara sempre uma cópia.

## Documentação {#documentation}

Este README é um ponto de partida. Os guias descrevem o comportamento completo e as limitações, pelo que as instruções detalhadas não precisam de ser mantidas duas vezes.

| Audiência | Inglês (EUA) | Francês |
|---|---|---|
| Usuários | [Guia do utilizador ilustrado](docs/USER_GUIDE.pt-PT.md) | [Guia ilustrado](docs/GUIDE_UTILISATEUR.md) |
| Desenvolvedores | [Arquitetura e Protocolo](docs/DEVELOPER_GUIDE.pt-PT.md) | [Arquitetura e protocolo](docs/GUIDE_DEVELOPPEUR.md) |
| Teste | [Validação](TESTING.pt-PT.md) | [Testes](TESTING.md) |

[Todas as edições HTML e Markdown](docs/README.pt-PT.md)

## Instalação {#installation}

### PPA Ubuntu

O [PPA do Gad](https://launchpad.net/~hizo/+archive/ubuntu/gad) permite instalar o Gad e receber atualizações através do APT:

```sh
sudo add-apt-repository ppa:hizo/gad
sudo apt update
sudo apt install gad
```

Verifique na página do PPA se existe um pacote publicado para a sua versão do Ubuntu. Se faltar `add-apt-repository`, instale `software-properties-common`.

### Transferências pré-compiladas

As [versões do GitHub](https://github.com/Hizoka76/gad/releases/latest) disponibilizam vários formatos. Substitua os nomes de exemplo pelos nomes dos ficheiros transferidos.

#### Pacote Debian `.deb`

Transfira o pacote para a sua arquitetura e instale-o com o APT para resolver as dependências disponíveis nos seus repositórios:

```sh
sudo apt install ./gad_VERSION_amd64.deb
```

#### Arquivo `.tar.gz`: fontes e binário compilado

O arquivo `gad-VERSION-linux-amd64.tar.gz` contém o código-fonte, as dependências Rust incluídas, a documentação e o programa compilado em `prebuilt/`. Não é necessário compilar para o executar:

```sh
tar -xzf gad-VERSION-linux-amd64.tar.gz
cd gad-VERSION
./prebuilt/usr/bin/gad
```

#### Executável direto

O ficheiro `gad-linux-amd64` também é um binário já compilado:

```sh
chmod +x gad-linux-amd64
./gad-linux-amd64
```

As transferências Linux amd64 destinam-se a processadores x86-64. O binário direto não instala um lançador nem o módulo Dolphin; o `.deb` e o arquivo incluem os ficheiros de integração dessa versão. Estes binários requerem Qt/KF6 nas versões indicadas nas notas de lançamento. Uma versão publicada corresponde à sua etiqueta e pode ser anterior ao código atual do repositório.

### A partir das fontes

No Linux, instale o Rust/Cargo, um compilador C++ 17, `pkg-config`, Qt 6 Widgets/Network/DBus e as ferramentas de construção Qt, KF6 WindowSystem/XmlGui, CMake e Python 3. O suporte da área de transferência do Wayland utiliza `wl-clipboard`; Os URLs remotos requerem fusível KIO. Veja o [guia do desenvolvedor](docs/DEVELOPER_GUIDE.pt-PT.md#building).

```bash
python3 scripts/install.py
gad
```

A instalação é feita por utilizador e não requer privilégios de administrador. Utilize `gad --background` para iniciar na bandeja do sistema.

## Cópia de Dolphin {#copying-from-dolphin}

1. Copie com **Ctrl+C** ou corte com **Ctrl+X** para mover ficheiros.
2. Clique com o botão direito do rato na pasta de destino e seleccione **Colar com Gad** (Colar com Gad).
3. Repita para adicionar outro pedido à fila.

A integração opcional de **Ctrl+V com o Gad no Dolphin** é ativada com `gad --configure --dolphin-ctrl-v` e desativada com `gad --configure --no-dolphin-ctrl-v`. Feche e volte a abrir o Dolphin para aplicar a alteração. No Gad, Ctrl+V cola no destino apresentado; arrastar e largar copia os ficheiros.

Para proteger o lançador habitual do Dolphin contra a perda do atalho, instale o módulo com o comando seguinte e feche e volte a abrir as janelas existentes uma vez. Verifica os atalhos a cada segundo, repara a ação do Gad e conserva a colagem nativa nos campos de texto.

```sh
python3 scripts/dolphin_integration.py --install --protect-launcher "$HOME/.local/bin/gad"
```


Para outros gestores de ficheiros: [colando em Gad, arrastar e soltar e ações personalizadas](docs/USER_GUIDE.pt-PT.md#using-gad-with-other-file-managers).

Desfazer depende do diário retido e dos backups do Gad. **Limpar operações concluídas** (Limpar operações concluídas) também remove o histórico de anulação. Leia o [guia do utilizador](docs/USER_GUIDE.pt-PT.md) antes de limpar o histórico de que pode necessitar.

## Desenvolvimento e validação {#development-and-validation}

```bash
make doctor
make test
make docs
```

Os testes utilizam ficheiros temporários e sessões isoladas. Não controlam a sua instância de ambiente de trabalho do Gad. [Testar comandos e requisitos](TESTING.pt-PT.md).
