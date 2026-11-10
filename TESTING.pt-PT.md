# Testando Gad {#testing-gad}

[Francês](TESTING.md) · [Guia do desenvolvedor](docs/DEVELOPER_GUIDE.pt-PT.md) · [Documentação](docs/README.pt-PT.md)

## Executando validação {#running-validation}

Da raiz do repositório:

```bash
make doctor           # Check tools, native libraries, and Rust components
make test             # Formatting, Clippy, Rust, Python, Qt, and integration
```

Sem fazer:

```bash
python3 scripts/run_tests.py all
python3 scripts/run_tests.py all --keep-going
python3 scripts/run_tests.py all --list
```

O runner retorna zero apenas quando todos os estágios necessários passam. Normalmente pára à primeira falha. `--keep-going` também executa estágios posteriores independentes; a integração permanece dependente de uma construção bem-sucedida.

## Grupos de teste {#test-groups}

| Comando | Cobertura |
|---|---|
| `make test-unit` | Testes de unidade Rust e Python. |
| `make test-python` | Caminhos XDG, integração Dolphin, executor e lógica de geração de documentação. |
| `make test-qt` | Área de transferência, widgets, conflitos, menus, relatórios de erros e notificações. |
| `make test-integration` | Crie e teste o executável real e o protocolo local. |
| `make check` | Formatação e Clippy com avisos tratados como erros. |
| `make test-bench` | Comparativo de mercado explícito de atualização de grande histórico. |
| `make docs-check` | Links de documentação existente, capturas e exportações bilingues. |

Os nomes dos grupos de executores são `unit`, `python`, `qt`, `integration`, `lint`, `bench` e `all`.

```bash
python3 scripts/run_tests.py unit --filter rollback_
python3 scripts/test_qt.py --suite row_actions
python3 scripts/test_qt.py --suite clipboard --suite notifications
cargo test --locked engine::tests::copies_tree_and_symlink_without_following_it
```

O filtro do executor aplica-se aos testes Rust; Os testes Python ainda são executados. O benchmark é ignorado durante a validação normal. Os scripts de invólucro individuais permanecem disponíveis.

## Relatórios {#reports}

Cada invocação do executor escreve o seguinte em `target/test-results/`:

- Um ficheiro `.log` para cada estágio executado.
- `results.json`, incluindo argumentos de comando, códigos de saída, estado e tempo decorrido.
- `junit.xml`, com **uma entrada por estágio de comando**, e não um por teste Rust/Qt individual. Os casos e asserções ao nível da estrutura permanecem nos registos de estágio.

```bash
python3 scripts/run_tests.py all --report-dir /tmp/gad-results
```

JSON e JUnit descrevem a execução mais recente e identificam os seus registos. Podem ainda existir logs de estágios ignorados mais antigos. `target/` é eliminado dos ficheiros de origem por `.gitignore`.

## Requisitos {#requirements}

- Linux e Rust/Cargo recente, com Rust 1.85 ou mais recente, Rustfmt e Clippy.
- Um compilador C++ 17, `pkg-config`, Qt 6 Widgets/Rede/DBus/Teste.
- `qmake6`, `moc`, `rcc`, KF6 WindowSystem e `dbus-run-session`.
- Python 3. Os testes de script de rotina utilizam apenas a sua biblioteca padrão.

O ambiente Debian Trixie CI fornecido instala:

```bash
sudo apt install build-essential pkg-config python3 cargo rustc rustfmt rust-clippy \
  qt6-base-dev qt6-base-dev-tools qt6-qpa-plugins qmake6 libkf6windowsystem-dev dbus fonts-dejavu-core
```

Utilize pacotes correspondentes noutras distribuições. `make doctor` identifica ferramentas em falta. A exportação de documentação em separado requer o Markdown, Graphviz; não são necessários para os sete testes de unidade de documentação padrão. Veja o [índice de documentação](docs/README.pt-PT.md#regeneration).

## Isolamento {#isolation}

Os testes do sistema de ficheiros utilizam árvores temporárias. O Qt é executado fora do ecrã com HOME/XDG isolado e uma sessão D-Bus privada. Os testes não instalam Gad, modificam a área de transferência real do utilizador, reiniciam a instância da área de trabalho ou operam sobre ficheiros de transferência pessoal.

As aberturas da área de trabalho são intercetadas através do `QDesktopServices`; nenhum editor ou gestor de ficheiros real é iniciado. As notificações KIO e KDE utilizam serviços falsos. A integração apenas controla o seu próprio executável isolado. Os servidores FTP/SFTP/SMB reais são desnecessários para o conjunto normal.

A remoção de dispositivos é simulada através da renomeação de diretórios âncora temporários, nunca desconectando ou desmontando um disco real. As tabelas de montagem simulada testam a lógica de montagem ligada sem privilégios de montagem.

## Adicionar um teste de regressão {#adding-a-regression-test}

| Área | Localização e abordagem |
|---|---|
| Motor Rust | Adicione as caixas `#[test]` em `src/engine/tests.rs`; utilize `Fixture`, aguarde os estados de negócio e inspecione o conteúdo do ficheiro, bem como JSON. Conclua o trabalho e desligue os novos motores sempre que possível. |
| Ações de linha/menu/abrir | Adicione slots ou linhas de dados em `tests/row_actions.cpp`; simule o mecanismo e intercete as aberturas da área de trabalho. |
| Comportamento da área de transferência e da janela | Estenda `tests/clipboard.cpp`. |
| Progresso do D-Bus | Estenda `tests/notifications.cpp` com o servidor JobView falso privado. |
| Scripts Python | Adicione `tests/python/test_*.py` utilizando `unittest`, `TemporaryDirectory` e simulações de ambiente/subprocesso. A descoberta é automática. |
| Novo conjunto Qt | Adicione o seu nome a `SUITES` em `scripts/qt_tests.py`, a sua fonte C++ e o moc gerado correspondente include. |

Teste o comportamento observável: estados resultantes, bytes corretos, originais retidos, destinos restaurados, argumentos de pedido de ambiente de trabalho e ausência de alterações não intencionais. Prefira um predicado sinal/estado a atrasos arbitrários. Não teste controlando a aplicação de desktop real.

## Integração contínua {#continuous-integration}

O `.github/workflows/tests.yml` fornece validação completa num contentor Debian Trixie e retém relatórios sobre falhas. Torna-se ativo quando o projeto é alojado num repositório GitHub adequado. A configuração é fornecida; nenhuma execução remota de CI é aqui reivindicada.

## Movimentos locais rápidos e montagens ligadas {#fast-local-moves-and-bind-mounts}

```bash
python3 scripts/run_tests.py unit --rust-filter fast_
```

Os casos `fast_move` inspecionam os inodes após transferência e desfazer, execução juntamente com a cópia, caminhos independentes partilhando uma pasta de destino, reservas de nomes após conflitos, caminhos sobrepostos, fallback MD5, alvos simultâneos, reprogramação EXDEV, cancelamento, cadeias de substituição e recuperação de diário. A modificação do mesmo comprimento com o mtime restaurado deve ainda fazer com que o desfazer e recusar um resultado alterado.

Os dispositivos rápidos utilizam tmpfs Linux graváveis ​​em `/dev/shm`, pelo que um `/tmp` com suporte de sobreposição em CI não desativa acidentalmente o caminho de renomeação local. O comportamento de renomeação injetado simula o EXDEV sem mover ficheiros para outro volume.

O `fast_bind_mounts` utiliza tabelas de montagem sintéticas para visualizações de ligação distintas e a sua visualização subjacente comum. Os testes cobrem a tradução nos dois sentidos, faltando a exposição comum, sistemas de ficheiros diferentes, restrições de leitura apenas e sufixos de destino que ainda não existem.

`fast_tree` adiciona preservação de inodes/permissão de raiz inteira, pastas vazias, múltiplas raízes, pausas direcionadas, desfazer parcial, ficheiros externos adicionados posteriormente, conteúdo modificado e recuperação antes/depois da publicação raiz. As montagens aninhadas, incluindo as ocultas numa visualização comum, impedem o atalho.

## Recuperação parcial após interrupção {#partial-recovery-after-interruption}

```bash
cargo test --locked resume_
python3 scripts/test_resume_integration.py
```

Crie o executável antes de executar um script de integração independente. Os testes abrangem a ordenação de pontos duráveis, validação de propriedade/versão de origem e temporária, comparação de prefixos, reconstrução MD5, truncamento de bytes finais apenas após validação e identidade temporária preservada. O conteúdo ou metadados alterados devem falhar sem truncar um ficheiro questionável.

A integração real-binária utiliza um processo isolado, SIGKILL, reinicialização e comparação de bytes/resumo para verificar se o mesmo ficheiro parcial é retomado. Nunca mata a instância Gad do utilizador. O trabalho explicitamente cancelado não deve tornar-se trabalho automático recuperável.

## Destinos remotos {#remote-destinations}

```bash
make test-remote
```

Este teste de rede real opcional requer `pyftpdlib`, fusível KIO e `/dev/fuse` utilizável. A validação normal utiliza serviços KIO falsos. O servidor opcional e os ficheiros permanecem num ambiente temporário; não é um teste contra uma conta remota pessoal.

Verifique a validação de URL, mapeamentos de endpoint, opções de operação/origem, metadados de serviço não suportados, semântica de sincronização e recuperação de destino KIO durável desativada. Os testes não devem tratar as propriedades ou capacidades indisponíveis como garantias de sucesso fabricadas.

## Dolphin Ctrl+V e progresso do ambiente de trabalho {#dolphin-ctrlv-and-desktop-progress}

```bash
make test-dolphin-paste
```

O teste de integração opcional requer os ficheiros de desenvolvimento CMake, Qt 6 Widgets/Test e KF6 XmlGui. Utiliza KXMLGUI/KActionCollection real em ambientes isolados HOME/XDG, D-Bus e área de transferência fora do ecrã. As ações são contadores: não ocorre nenhuma cópia real ou reinicialização do utilizador Dolphin.

Verifica o Gad Ctrl+V, a preservação da alternativa nativa Shift+Insert KIO, a colagem do campo de texto nativo, as definições de Ctrl+V restauradas após a desativação, a validação completa antes de modificar a definição, a rejeição de recibos inválidos e a reversão após falhas de escrita.

Os testes da área de transferência injetam respostas de sessão Dolphin para janelas ativas exclusivas, aliases de serviço, espaços e `#`, títulos curtos, pastas fechadas, alterações de navegação/foco, serviços indisponíveis e URLs remotos. Os testes Python cobrem a migração/restauração de integração sem alterar ações não relacionadas.

Os testes de notificação exercitam os comandos que regressam através do controlador orientado para o motor, etiquetas de pausa sincronizadas/estado da bandeja, uma única vista de progresso combinada, velocidade em detalhe, contabilização de conclusão do MD5, reconexão de serviço e a transição de `suspended` de volta para falso. As notificações comuns não devem duplicar a visualização nativa do trabalho.

## Verificações de espaço livre, prioridades e árvores {#free-space-checks-priorities-and-trees}

Os testes de espaço abrangem orçamentos partilhados por volume, volumes separados, arredondamento de blocos, overflow, ficheiros parciais alocados, persistência de opções e renomeações rápidas. Um dispositivo tmpfs esparso cria uma escassez sem preencher o disco. A capacidade remota simulada desconhecida avisa e é concluída. Qt verifica as opções capturadas e um aviso permanece visível após um resultado OK.

Os testes de prioridade utilizam o agendador real para pedidos estáveis ​​de vários pedidos, trabalho pausado, rejeição atómica de seleção obsoleta e desfazer correto após substituições reordenadas. A integração IPC reordena e restaura os pedidos em fila antes de prosseguir com as cópias.

Os testes de widgets verificam a floresta sem cabeçalho, acentos de pedido partilhados em temas claros/escuros, seleção de pedido inteiro, referências de arrasto internas estáveis, rejeição de arrasto estrangeiro, paternidade/destinos preservados e estabilidade de seleção/focagem/expansão. Os testes de mecanismo abrangem raízes independentes alternadas, ordem de irmãos exposta por conflitos, reparação de parentesco inválida, prioridades de espaço reservado e dependências de substituição durante a pausa/finalização.

Os objetos Qt nativos são armazenados em cache de conteúdo em `target/qt-test-cache` com origem/cabeçalho, versão Qt/KDE e hashes de identidade do compilador. Um bloqueio de construção impede a construção simultânea da cache. Os executáveis ​​de teste, ficheiros, configuração e sessões D-Bus permanecem atualizados e isolados.

As dependências de destino normalizadas incluem aliases pai, nomes repetidos, pausas seletivas e futuras reservas de nomes de cópia. Os avisos de propriedade de pasta resumem apenas os seus próprios descendentes reais.

## Supressão de transferência duplicada {#duplicate-transfer-suppression}

Os casos `duplicates_` cobrem pastas pausadas repetidas, preparação simultânea, duplicados dentro de um envio, pastas sobrepostas, aliases de pais e índices estáveis ​​restaurados. Preservam destinos distintos, intenções de copiar/mover, entradas de diretório de ligação física com nomes diferentes e novos pedidos intencionais após conclusão/cancelamento.

Qt verifica uma única mensagem informativa de atualizações apenas de metadados sem pedidos repetidos. Os cenários de conflito utilizam fontes genuinamente distintas que partilham um nome de ficheiro, pelo que testam conflitos em vez de supressão duplicada.

## Limites e estimativas de largura de banda {#bandwidth-limits-and-estimates}

O `engine::rate` utiliza um relógio injetado para verificar orçamentos partilhados/globais e por solicitação, crédito inativo limitado, consumo agregado e alterações em tempo real. Os testes de mecanismo realizam uma cópia real limitada, removem o limite e verificam a saída. São abordados a pausa, o cancelamento de baixa taxa, a validação atómica, a persistência e a preservação de um limite mais recente durante a preparação de novas tentativas.

Qt verifica a conversão de MiB em bytes e um alvo por pedido de propriedade, mesmo quando várias das suas linhas são seleccionadas. Alterar a seleção enquanto a caixa de diálogo de taxa está aberta não deve alterar os seus IDs capturados.

Os testes de métricas determinísticas cobrem taxas/tempos de pedido simultâneos independentes, fases de cópia versus MD5, pausas/conflitos, renomeações, prefixos retomados, redefinições de contadores, progresso paralisado e formatação de duração. O widget de resumo real é atualizado em `poll.unchanged` sem substituir linhas ou a sua seleção/dicas de ferramentas.

## Reconexão do dispositivo {#device-reconnection}

Os casos `reconnect_` simulam o desaparecimento da origem/destino através da renomeação de âncoras temporárias. Cobrem a recuperação de prefixo durável validado, pausas retidas, nenhuma gravação num diretório de substituição vazio, recusa de fonte alterada, cancelamento e trabalhadores libertados para trabalho independente.

Casos adicionais abrangem a reinicialização de um ficheiro não publicado de propriedade sem um ponto durável, recusa de reprodução de publicação, alterações de número de dispositivo autorizadas por UUID, resumos de metadados/conteúdo preservados durante a religação e comandos simultâneos de pausa/cancelamento durante a preparação. Nenhum sistema de ficheiros real é montado ou desmontado.

Qt verifica as etiquetas de linha e de progresso, a disponibilidade de cancelamento, a visualização de trabalho nativa suspensa e a resolução KIO assíncrona que não adiciona trabalho nem aumenta a janela.

## Relatórios de erros e novas tentativas {#error-reports-and-retries}

Os testes `errors_` abrangem relatórios de leitura apenas, falhas de preparação/finalização, validação atómica, índices obsoletos, referências exatas de relatórios, preparação de novas tentativas reparadas e proteção de saída publicada modificada. Um pedido real com falha parcial verifica se o conteúdo, o inode, os carimbos de data e hora e o diário do vizinho bem-sucedido permanecem inalterados após uma nova tentativa.

A pausa global, as entradas canceladas/ignoradas e os trabalhos pendentes não selecionados são preservados. A agregação de diretórios evita registos de erros duplicados, ao mesmo tempo que retém falhas reais de pastas.

Qt verifica a filtragem e os destinos de pedido estáveis ​​após classificação, ações de repetição contextuais, relatórios vazios, texto UTF-8 completo, JSON analisável e gravações de relatórios atómicos. As exportações utilizam apenas caminhos temporários. Cada botão da caixa de diálogo de erro é verificado com um tema de ícone deliberadamente indisponível para exercitar os ícones de substituição Qt padrão.

## Geração da documentação {#documentation-generation}

```sh
make docs-captures
make docs
make docs-check
```

Os testes da documentação verificam o catálogo de seis idiomas e os três tipos de guia, a navegação entre idiomas, os formatos de papel, as legendas e as imagens incorporadas. Também verificam o formato compacto do início rápido, as capturas próprias de cada idioma e a preservação do código dos exemplos.

A geração produz diagramas traduzidos e 18 guias. Em seguida, verifica ligações e âncoras, as 84 capturas com tema claro e as suas dimensões e assinaturas, o idioma do HTML, as imagens incorporadas e as 36 exportações HTML/Markdown. Requer as dependências opcionais indicadas no índice da documentação.

[Documentation](docs/README.pt-PT.md#regeneration)

## Traduções de aplicações {#application-translations}

Cinco testes Python verificam a cobertura da string Qt marcada, impressões digitais TS/QM, eliminação de comentários e identificadores não marcados, traduções em falta e preservação de parâmetros e ligações. Os testes Qt carregam o catálogo francês incorporado real e verificam widgets, estados, preferências de idioma, relatórios de texto e caminhos inalterados/JSON. A exportação de relatórios vazios e as transições de atualização/filtro também são abordadas. Execute `make translations-check` para uma verificação independente.

O Qt de idioma ao vivo testa o francês e o inglês dos EUA alternados durante uma transferência ativa com uma resposta de pesquisa inalterada. Verificam a identidade da linha, a seleção, a expansão, as entradas de destino/taxa, os diálogos Sobre/erro/relatório/taxa, as opções de conflito, as contagens de ligações estáveis ​​e a ausência de comandos de gravação do motor.

## Exclusões e itens ocultos {#exclusions-and-hidden-items}

Os testes Rust cobrem os padrões Unicode, validação atómica, padrões legados, remoção de pastas, ficheiros temporários, links simbólicos, persistência de regras por trabalho, novas tentativas, pedidos totalmente eliminados e movimentos seguidos de desfazer. Os testes Qt abrangem preferências guardadas, edições canceladas e rejeitadas, tradução ao vivo e captura de regras para novos pedidos.

```sh
cargo test --locked exclusions
python3 scripts/test_qt.py --suite row_actions
```

## Integração Konqueror {#konqueror-integration}

`python3 scripts/test_qt.py --suite konqueror` verifica a pasta atual, URLs codificados e remotos, preservação em modo de corte, colagem de texto nativo e Shift+Insert num ambiente isolado. O plugin foi também carregado no Konqueror 26.08.2 extraído para um diretório temporário com HOME/XDG e D-Bus privados; foi verificado o URL exato da pasta ativa. Os registos locais estão em `target/konqueror-real-test.log`.

## Diálogo de definições {#settings-menu}

Os testes Qt verificam tamanhos mínimos, os cinco separadores, Fechar e Esc, conservação dos controlos, tradução imediata e o botão de velocidade na barra de ações. A CLI é testada com valores inválidos, opções contraditórias, exclusões parciais e sincronização de preferências. Python verifica os seis manuais, os seis idiomas, a instalação e a rejeição de traduções em falta.

```sh
make manpages-check
make test-cli-settings
```

## Vigilância automática de Ctrl+V {#dolphin-watchdog}

Os testes Qt abrangem a recriação de ações, o temporizador real, a desativação, o restauro da colagem nativa, alterações do utilizador, diálogos modais, registos inválidos e ações em falta. A colagem de texto permanece nativa. Python verifica a proteção reversível do lançador, os argumentos e alterações posteriores. O teste com Dolphin real atualiza os menus e aciona Ctrl+V: um comando fictício regista o arranque sem copiar ficheiros nem aceder ao ambiente de trabalho.

```sh
make test-dolphin-watchdog
make test-dolphin-watchdog-real
```


## Verificações recentes da interface

`exclusionsTabCommitsOnFocusLossAndTranslates` e `exclusionsSaveWhenFocusLeavesTheField` verificam a gravação ao perder o foco, a rejeição sem perder o texto, campos independentes e o fecho das opções. `clipboardAdditionKeepsPreparedQueueWaiting` verifica que os itens da área de transferência não iniciam trabalho preparado, pausado ou histórico. Também são testados Copiar/Recortar, colagem externa automática, Ctrl+O/Ctrl+Shift+O/Ctrl+Q/Ctrl+Alt+W e as cinco abas.
