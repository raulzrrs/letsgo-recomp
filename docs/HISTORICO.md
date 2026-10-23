# Histórico do projeto: o que deu errado e como foi resolvido

Este documento conta, em ordem, como o port do **Pokémon Let's Go Eevee** saiu do zero até o jogo ser jogável
(começar um jogo novo, capturar o Pikachu, salvar e carregar). Cada problema aparece com o **sintoma** (o que se via),
o **diagnóstico** (como a causa foi encontrada), a **causa** e a **correção**, com os arquivos alterados.

Período: 14 e 15/10/2026. Estado atual e pendências: [PROGRESSO.md](PROGRESSO.md). Ferramentas de diagnóstico:
[DEPURACAO.md](DEPURACAO.md).

---

## Parte 1: escolha do caminho

### A ideia
Rodar o jogo do Switch **nativamente no PC**, sem emulador, usando **recompilação estática**: o código ARM64 do jogo
é traduzido para C uma única vez, compilado como um programa Windows, e um *runtime* substitui o sistema do Switch
(kernel, serviços, GPU, áudio, controles).

### Por que o LASRecomp como base
- O [LASRecomp](https://github.com/peiikai/LASRecomp) (Link's Awakening, Switch, licença MIT) já tinha provado que
  o caminho funciona no Switch: tradutor ARM64 → C, kernel/serviços do Horizon e a API gráfica **NVN** traduzida
  para Vulkan.
- A análise ([ANALISE-LASRecomp.md](ANALISE-LASRecomp.md)) mostrou que o núcleo é genérico e que o específico do
  Zelda fica em camadas separáveis (patches por endereço, randomizer, interface).

### Alternativas consideradas
| Alternativa | Decisão | Motivo |
|---|---|---|
| Começar um recompilador do zero | não | o LASRecomp já resolve a parte genérica |
| HWDER (Hyrule Warriors) | plano B para gráficos | recompila o driver NVN e emula comandos Maxwell; licença não declarada, só estudo |
| ssbu-recomp (Smash) | só estudo | GPL-3, incompatível com copiar código para um projeto MIT |
| mk8-recomp | descartado | depende do suyu em execução |
| Reestruturar com CMake (sugestão externa) | não | o Makefile do LASRecomp já funciona; reorganizar não aproxima o objetivo |

### Extração do jogo (regras)
- O jogo vem do **cartucho do próprio usuário** (XCI). Extração com `hactool` usando o `prod.keys` **gerado no
  console do usuário** (Lockpick_RCM). Chaves baixadas de sites foram recusadas: a verificação por hash mostrou que
  as primeiras chaves oferecidas eram idênticas ao pacote do ProdKeys.NET; o dump verdadeiro tinha hash diferente e
  chaves exclusivas do console (`device_key_4x`, `sd_seed`, ...).
- Resultado em `extracted/` (fora do git): `exefs/` (código), `romfs/` (assets) e `romfs.bin` (imagem RomFS crua,
  gerada pelo `hactool --romfs`).
- **Nada do jogo entra no repositório:** arquivos do jogo, chaves, código C gerado, logs, saves e caches estão no
  `.gitignore`. O harness de shader (`tools/skintest`) gera o código derivado do jogo só localmente.

---

## Parte 2: diagnóstico do binário

Detalhes em [DIAGNOSTICO.md](DIAGNOSTICO.md). Em resumo:

- 4 módulos: `main` (o jogo), `sdk` (bibliotecas da Nintendo), `subsdk0` (**glslc**, compilador de shaders) e
  `subsdk1` (**multimedia**, vídeo). O Zelda só tem `main` + `sdk`.
- O tradutor do LASRecomp converteu **100% das instruções** dos 4 módulos (0 não suportadas).
- Title ID confirmado no `main.npdm`: `0100187003A36000`.

---

## Parte 3: montar o fork

1. **Ambiente:** MSYS2 UCRT64 (clang, SDL3, FFmpeg, Vulkan, shaderc) instalado via winget/pacman; Python com
   capstone/unicorn/lz4 em `.venv`.
2. **Remoção da camada do Zelda:** 16 arquivos de HLE de gameplay/randomizer e 11 telas de interface removidos.
   O que o resto do runtime ainda chamava virou stub neutro em `runtime/hle/zelda_stubs.c`.
3. **4 módulos em vez de 2:** loader, tabela de despacho, biblioteca do jogo (`lasgame.dll`, ABI 2) e Makefile
   passaram a tratar `main`, `sdk`, `subsdk0` e `subsdk1` (`runtime/loader.c`, `dispatch.c`, `gamelib.*`,
   `tools/gamelib_entry.c`, `gamebuild.c`, `rt.h`).
4. **Gerador sem os patches do Zelda:** `tools/patches.py` esvaziado (os patches eram endereços do executável do Zelda).

Primeiro build: runtime em segundos; código do jogo (~550 MB de C) em ~22 minutos.

---

## Parte 4: do boot até a tela de título

| # | Sintoma | Diagnóstico | Causa | Correção | Arquivo |
|---|---|---|---|---|---|
| 1 | Crash nos construtores do `main` (acesso a `0x1c0`) | disassembly: `nn::os::SetDyingMessageRegion` grava em `[GetInfo(20)] + 0x1c0` | `svcGetInfo` id 20 (contexto de exceção do usuário) devolvia 0 | aloca uma área e devolve o endereço | `runtime/kernel.c` |
| 2 | — (encontrado no caminho) | código do `svcGetInfo` | Title ID do Zelda fixo no kernel | lido do ACI0 do `main.npdm` | `runtime/main.c`, `kernel.c` |
| 3 | Crash em `nn::vi::GetService` | addr2sym no backtrace | `nn::vi::SetLayerScalingMode` sem HLE (o Zelda não usa) | HLE da função | `runtime/hle/vi.c` |
| 4 | Jogo aborta (`svcBreak`) | backtrace nomeado: `nn::btm::user::InitializeBtmUserInterface` | serviços `btm:u`/`bt` eram stubs genéricos sem objetos/eventos | serviços "offline" (Bluetooth sem dispositivos) + `csrng` com números aleatórios | `runtime/services/offline.c` |
| 5 | Jogo parado, nenhum frame | novo dump de threads (`LARECOMP_THREAD_DUMP`) | deadlock de mutex: no timeout de condvar o kernel readquiria o mutex, e o SDK (`TimedWait` → `Enter` em `0xea01`) também | kernel não readquire no timeout | `runtime/kernel.c` |
| 6 | Crash em `nvnCommandBufferBindProgram` | símbolo do runtime via `nm` | o jogo faz bind de programa NULL no início | (depois substituído pelo item 22) | `runtime/nvn/cmdbuf.c` |
| 7 | "Press a button on the controller you want to use" sem aceitar o controle | captura de frame (`LARECOMP_SHOT_AT`) + `SetSupportedNpadStyleSet 0x5a` | o runtime apresentava um **Pro Controller**, que o Let's Go não aceita | controle como **modo portátil** (configurável por `LARECOMP_NPAD`) | `runtime/hle/input.c`, `services/hid_set.c` |
| 8 | Crash ao abrir o vídeo de título | backtrace: `movie::MediaData::FindInt64` | 2 das 28 funções de vídeo sem HLE (já previsto no diagnóstico) | `FindInt64` e `SetCoreMask` | `runtime/hle/movie.c` |
| 9 | ~240 fps, áudio picotado | estatística do renderer | o runtime ignorava o *present interval* do jogo | espera pelo intervalo do jogo (2 = **30 fps**), `LARECOMP_FPS` para mudar | `runtime/nvn/objects.c` |

### O vídeo de título (o caminho mais longo)
| # | Sintoma | Diagnóstico | Causa | Correção |
|---|---|---|---|---|
| 10 | GPU "device lost" (escrita no endereço 0) | camada de validação do Vulkan | texturas do vídeo eram encolhidas pela **escala de resolução interna** e a cópia em tamanho cheio estourava | texturas preenchidas pela CPU ficam em tamanho cheio (`vk_tex_unscale`) |
| 11 | Tela verde (NV12 com Y=UV=0) | testes por eliminação: sem escala, buffers no heap do jogo, preencher com cinza → continuava verde | — (hipóteses descartadas, mas viraram melhorias: `backend_pool_written`, alocador do jogo) | ver item 12 |
| 12 | Tela verde | log de quem lê os quadros: o jogo nunca chamava `Buffer::Base` no vídeo; `GetOutputBuffer` recebia um **buffer do próprio jogo** (1920×1080×1,5) | o runtime trocava o buffer do jogo pelo seu; o jogo continuava lendo o seu, vazio | o quadro é copiado no buffer do jogo, em NV12 compacto |
| 13 | Áudio do vídeo "robótico" | gravação WAV da saída + comparação com o arquivo original (70% das amostras exatas) + teste de ouvido do usuário | o decoder reusava na hora o buffer de saída recém-liberado, enquanto o jogo ainda copiava dele | slots de saída em rodízio |
| 14 | Música e efeitos mudos (saída de 6 canais toda em zero) | gravação WAV por saída | `hwopus` (decodificador Opus) era stub: o jogo desistia | serviço `hwopus` implementado com o Opus do FFmpeg |
| 15 | Vídeo congelava em quadro aleatório | dump de threads: thread de vídeo esperando um mutex **dela mesma** | corrida no kernel: um sinal chegava junto com o timeout, o kernel entregava o mutex e mesmo assim respondia "timeout" | mantém o resultado de quem acordou a thread |
| 16 | O jogo nunca saía do vídeo (tela branca no fim) | estado dos decoders + contagem de pacotes do arquivo (3.458) | no fim do arquivo o extrator devolvia `1` (erro genérico, "tente de novo") | devolve `0x2000`, o "fim de stream" de verdade, achado na própria biblioteca do jogo (`NnMediaCommon::AndroidErrorToNnError`) |

Arquivos: `runtime/hle/movie.c`, `runtime/nvn/vulkan/textures.c`, `exec.c`, `runtime/services/hwopus.c`,
`runtime/services/audio.c`, `runtime/kernel.c`.

---

## Parte 5: introdução, 3D e gameplay

| # | Sintoma | Diagnóstico | Causa | Correção | Arquivo |
|---|---|---|---|---|---|
| 17 | Nome "RRS" aparecia como "?S" | bytes: `52 52 53` lidos como UTF-16 = `U+5252` + `S` | o teclado devolvia UTF-8; o Let's Go lê UTF-16 | resposta em UTF-16LE | `runtime/hle/swkbd.c` |
| 18 | Sem 3D (tela preta com som no quarto) | contagem de draws pulados por motivo: todos "noobj" | o jogo faz bind de **programas separados por estágio** (vertex num, fragment noutro, NULL para desligar); o runtime trocava o programa inteiro a cada bind | programa combinado por estágio (`bind_program_stages`) | `runtime/nvn/cmdbuf.c`, `vulkan/exec.c` |
| 19 | ~4.000 draws/2 s ainda pulados | log dos estágios: `0x1` | programas só com vertex shader (passes de profundidade/sombra) | fragment shader vazio sintetizado | `runtime/nvn/vulkan/exec.c` |
| 20 | Mira da Poké Ball ia para o céu | — (relato do usuário) | sensor de movimento (`SixAxisSensor`) sem implementação: o SDK lia lixo | sensor neutro (parado, nivelado, apontado para a frente) | `runtime/hle/input.c` |

Resultado: **cenário 3D correto** (quarto, Pallet Town, céu), captura do Pikachu, **save e load funcionando**
(`save/save_0/savedata.bin`).

---

## Parte 6: personagens "explodidos" (resolvido)

Os modelos com esqueleto (personagens, Pokémon) apareciam esticados em "espinhos" que convergiam para um ponto.
O cenário estático estava correto, então câmera e geometria comum funcionavam. Plano e experimentos completos em
[PLANO-3D.md](PLANO-3D.md).

| # | Sintoma | Diagnóstico | Causa | Correção | Arquivo |
|---|---|---|---|---|---|
| 21 | Personagens e Pokémon em espinhos | captura no RenderDoc + o shader rodado na CPU com os mesmos dados da GPU: saídas idênticas, e o `w` de saída proporcional ao peso do **1º** osso | `LDC.64 R4, c[4][R4+0x38]` (carrega R4 e R5 com R4 como endereço) virava `r4 = cb4[r4+56]; r5 = cb4[r4+60]`: a segunda leitura usava o R4 **já sobrescrito**. O `w` da translação do 2º osso (que deveria ser 1,0) vinha zero ou lixo, e o vértice ia para perto do plano da câmera | o tradutor copia o registrador de endereço para uma temporária quando ele também é destino | `runtime/nvn/maxwell/translate.c` |

### O caminho até a causa
1. **Descartados com dados** (antes do plano): formato de atributo, índices e pesos em memória, matrizes da paleta,
   momento da cópia dos uniforms, cópia dos vértices.
2. **Fases 0 e 1** do plano: reprodução determinística com o save (`tools/repro.sh`) e cortes binários na GPU. Peso
   100% no 1º osso deixava os modelos inteiros; o 2º osso estava errado. A conclusão da época, "o valor de `a11.y`
   chega errado na GPU", estava **errada**: o teste do shader na CPU (X5) tinha passado por coincidência, porque o
   vértice usado não exercitava a leitura defeituosa.
3. **Fase 3, RenderDoc**: os bytes de vértice que a GPU lê (incluindo `a11 = [7, 6]` e os pesos) eram idênticos aos
   da memória. Rodar o shader traduzido na CPU com os dados **exatos** da GPU (vértices e `cb3`/`cb4` exportados da
   captura) reproduziu a saída da GPU número por número. Então o defeito estava no próprio shader traduzido. O log
   das leituras de `cb4` no harness e a leitura do GLSL apontaram as duas linhas acima.

Enquanto a correção não existia, o `jogar.bat` usou um paliativo (`LARECOMP_VK_SKIN_ONEBONE=1`, peso 100% no 1º
osso: modelos inteiros, animação mais rígida).

---

## Parte 7: interface

| # | Sintoma | Diagnóstico | Causa | Correção | Arquivo |
|---|---|---|---|---|---|
| 22 | Ícones dos Pokémon "picotados" (equipe, tela de experiência após captura, Pokédex) | log das texturas do draw (`68x56`, formato `0x3B`) e decodificação da textura gravada com 4 layouts de bits: só "R nos bits baixos, alfa no bit 15" dá o ícone certo | o NVN `RGB5A1` (e o `RGBA4`) tem o vermelho nos bits baixos; o Vulkan `R5G5B5A1_UNORM_PACK16` tem o vermelho nos altos e o alfa no bit 0: cores trocadas e alfa tirado de um bit de cor | convertidos para RGBA8 no upload, como o RGB8 | `runtime/nvn/vulkan/textures.c` |
| 23 | Flash azul e depois preto ao abrir o jogo | prints dos primeiros frames e log das limpezas (`LARECOMP_LOG_CLEARS`, agora com a cor): os 2 primeiros presents são um quad 2D azul de tela cheia desenhado pelo próprio jogo, sem nenhum clear; o `nn::oe::FinishStartupLogo` é chamado já no frame 0, então o logo do sistema não os esconderia | não esclarecida (não se sabe se o console mostra o mesmo) | a janela fica preta nos 2 primeiros presents (`LARECOMP_HIDE_FIRST_FRAMES`, 0 desliga) | `runtime/nvn/vulkan/device.c` |
| 24 | Alguns objetos nunca apareciam (1247 draws pulados como `failed` em 4 min de jogo) | `tools/perf_summary.py` no log de uma sessão longa; o log tinha `FS translate failed: unknown instruction df5800812ff70400` | o tradutor de shaders não conhecia a `TMML` (consulta do nível de mipmap que uma amostra usaria): o shader inteiro falhava e todo draw com ele era pulado. O shader do jogo usa o nível para limitar a amostra (`min(lod, 2)`) | `TMML` traduzida com `textureQueryLod` (ponto fixo 8.8, componentes na ordem inversa do GLSL, como no Ryujinx) | `runtime/nvn/maxwell/translate.c` |
| 25 | Objetos aparecendo de novo na primeira vez em cada área depois de um crash | data do `cache/vk_pipelines.bin` | o cache de pipelines só era gravado ao fechar o jogo normalmente | gravado numa thread de fundo ~6 s depois que uma leva de pipelines novos termina (~90 ms, sem travar quadro) | `runtime/nvn/vulkan/exec.c` |


## Parte 8: qualidade de imagem e pop-in (15/10/2026)

Auditoria de um quadro da Rota 1 (`LARECOMP_DEBUG_FRAME`, imagens de cada passe) e da camada de validação do Vulkan:
o quadro é mapa de sombras 2048² → cena HDR 1080p → cadeia de bloom (960x540 até 10x6) → composição → interface, sem
draw faltando. A validação acusa só uma textura RGBA8 16x16 (o "sem sombra" do próprio jogo) lida por um amostrador
de sombra: fora da especificação, mas a GPU NVIDIA compara o canal vermelho como o Tegra do Switch.

| # | O quê | Medição | Mudança | Arquivo |
|---|---|---|---|---|
| 26 | Serrilhado: o jogo não tem antisserrilhado | prints da janela com e sem | o SMAA 1x herdado do LASRecomp (opção do menu Esc do Zelda, removido) volta ligado por padrão, com a caixa "Suavizar serrilhado" no launcher (chave nova `smaa` no ini: o `antialias = off` antigo era só o padrão salvo) | `runtime/settings.c`, `runtime/ui/launcher_lg.mm` |
| 27 | Supersampling (1440p / 4K numa tela menor) reduzido com um só tap bilinear, que pula texels | sem limite de fps, gameplay a ~160 fps em 1080p, ~115 em 1440p e ~78 em 4K (RTX 3080): 4K numa tela 1080p sobra para 30 fps | a apresentação usa 4 taps bilineares espalhados na área de cada pixel da janela (em 2x exato, a média 2x2) | `runtime/nvn/vulkan/shaders.c` |
| 28 | Texturas borradas em ângulo rasante (caminhos, grama ao fundo) | o jogo pede pouca ou nenhuma filtragem anisotrópica | 16x nos amostradores com mipmap e filtro linear (`LARECOMP_ANISO`) | `runtime/nvn/vulkan/textures.c` |
| 29 | Objetos surgindo alguns quadros depois de cada leva de shaders novos | ~20-60 draws pulados por leva na gameplay; com cache, ~1 ms por programa | o draw espera o programa / pipeline ficar pronto, até 20 ms por quadro (a 30 fps sobram ~27 ms), em vez de ser pulado: zero draws pulados na gameplay, fps igual; os que sobram ficam na tela de carregamento (`LARECOMP_SHADER_WAIT_MS`) | `runtime/nvn/vulkan/exec.c` |
| 30 | Primeira visita a uma área: shaders prontos devagar | cache vazio (pasta `cache/` afastada): tradução ~240 ms numa thread só, enquanto as 16 threads de compilação esperavam; depois disso, o que pesa é o driver criando pipelines (~30 ms cada) | a tradução roda nas threads de trabalho (o buffer de strings `RSC2` do tradutor passou a ser por thread) e compila em seguida na mesma thread. Mesmo com cache vazio, toda a preparação cai nos quadros pretos do carregamento: a área aparece completa (prints da transição) | `runtime/nvn/maxwell/translate.c`, `runtime/nvn/vulkan/exec.c` |
| 31 | Mouse sem função; teclado com o layout do Zelda (K/J/L/I) | encontro achado com tentativas (`LARECOMP_GYRO_TEST`, o jogo só liga o sensor depois de "Get ready") | teclado com layout do Let's Go; mouse preso à janela com um clique: botões = A / B, movimento = giroscópio virtual (depois, #34: preso sozinho só durante a mira; fora dela é o dedo na tela de toque) | `runtime/hle/bindings.c`, `runtime/hle/input.c` |
| 32 | Mira da Poké Ball parada no modo portátil | o jogo leu o sensor só 2 vezes por captura: `IsSixAxisSensorAtRest` respondia sempre "parado" e o jogo pula a leitura de um sensor parado | nunca "parado" (um console na mão nunca está); orientação, ângulo, velocidade angular e gravidade coerentes; eixo z = mira horizontal, x = vertical. O jogo mede o giro a partir do início da mira e ignora saltos bruscos (ruído): só movimento contínuo, como o do mouse, mexe a câmera | `runtime/hle/input.c` |
| 33 | Sem como jogar traduzido (o jogo não tem português) | o runtime já tinha um LayeredFS completo (trocar e adicionar arquivos do romfs) usado pelas seeds do randomizer do Zelda, desligado no Let's Go; a tradução PT-BR de fãs vem como pasta do Atmosphere e troca os textos de `English` e `Spanish`, a interface (`bin/app`) e imagens | `runtime/mods.c`: a pasta `mods/<nome>/` escolhida no launcher alimenta o mesmo mecanismo (sem desviar os saves, como a seed fazia). O idioma pedido ao sistema virou configurável (`game_lang`), mas o jogo segue o idioma gravado no save: trocar só ele não muda os textos de um save existente | `runtime/mods.c`, `runtime/services/fs.c`, `runtime/ui/launcher_lg.mm` |
| 34 | "Brincar com Eevee" não respondia | o modo portátil acaricia pela tela de toque; o jogo importa `GetTouchScreenStates<16>` e `GetGestureStates`, nenhum dos dois implementado (lia memória vazia) | `GetTouchScreenStates` devolve um dedo: o mouse livre sobre a imagem do jogo (convertido para a tela de 1280x720 do Switch; a escala certa é a da TV, 1920x1080: #36), com Start / End no começo e no fim do toque; testado acariciando a testa: o Eevee fecha os olhos, notas musicais e brilhos. Gestos: o jogo consulta, mas o carinho não depende deles (devolve nenhum) | `runtime/hle/input.c`, `runtime/nvn/vulkan/device.c` |
| 35 | Mouse e teclado reais interferindo nos testes automáticos | posição do mouse do usuário aparecendo no log de um teste | o filtro `input_user_blocked` só existia no backend Metal; o laço de eventos do Vulkan não o chamava | `runtime/nvn/vulkan/device.c` |
| 36 | Botão de frutas do "Brincar com Eevee" não abria | toques no ícone com vários formatos (rápido, segurado, arrastado) e dia adiantado; o disassembly do leitor de toques do jogo (`0x70007c1ac4`) mostrou que ele confere a posição de cada amostra, inclusive a de soltar; com a coordenada em escala 1920x1080 o botão abriu | o runtime diz ao jogo que está na TV (1920x1080) e o jogo usa o toque nessa escala, não nos 1280x720 do portátil: o ícone ficava a 2/3 do lugar. Toques agora em pixels da tela informada (`LARECOMP_DISPLAY_W`), e a amostra de soltar repete a última posição (antes ia para a posição padrão / do mouse) | `runtime/hle/input.c` |
| 37 | Brincar com Eevee impossível só com controle | o modo portátil depende da tela de toque | dedo virtual: analógico direito move, ZR segurado toca (ZR não chega ao jogo, onde só repete o A), círculo desenhado pelo overlay (`runtime/ui/menu.mm`). Testado o fluxo inteiro: abrir as frutas, fechar o tutorial com A, pegar a Fruta de Razz, levar à boca: ×23 → ×22 | `runtime/hle/input.c`, `runtime/ui/menu.mm` |
| 38 | Restos do Link's Awakening no runtime | levantamento de cada função neutralizada em `hle/zelda_stubs.c` e de quem a chamava | o fork tinha desligado a camada do Zelda com stubs, mas as chamadas e as telas continuavam no código | removidos: os stubs e as chamadas (warp, pets, portas, potes dourados, garrafa, corações, tela larga das masmorras, editor de save, troca de nível por nome), o randomizer, o troca-ícones de botão por textura do Zelda (`texpatch.c`), as abas antigas do menu Esc (o menu agora só tem a pergunta de fechar, a digitação de nomes, o fps e o dedo do controle), o launcher antigo (de Mac), a montagem do código do jogo na máquina do jogador (`gamebuild.c`, zig) e o backend Metal de Mac, que nunca foi compilado neste projeto. As opções do Zelda saíram do `larecomp.ini` (as antigas são ignoradas) | `runtime/` |
| 39 | Jogar exigia MSYS2, `make gen`, `make` e um `jogar.bat` | comparação com outros ports nativos (Zelda64Recomp, UnleashedRecomp, o próprio LAS Recomp): um zip com o exe, a pasta do jogo escolhida na primeira tela, nada para instalar | o fluxo de release do LAS Recomp (gerador congelado, Zig, `gamelib-src/`) tinha sido removido junto com o resto do Zelda | `runtime/gamesetup.c` (a pasta escolhida no launcher: procura `exefs` e `romfs` / `romfs.bin`, confere o build id da v1.0.0, copia para `extracted/` e recompila o código do jogo com `runtime/gamebuild.c`, os 4 módulos); launcher em inglês ou português (idioma do Windows), com progresso e erros explicados; "Instalar tradução..." (`runtime/mods.c`: abre .zip/.rar/.7z com o `tar` do Windows e acha o romfs do Eevee); `make dist` (FFmpeg só LGPL, gerador congelado, Zig enxuto) → `LetsGoRecomp.exe`. Armadilha nos testes: o `timeout` do MSYS2 não repassa variáveis de ambiente a exes nativos |
| 40 | Na release 0.1.0, o jogo fechava logo depois da tela do controle (`svcBreak` 2111-0001) | teste da release baixada do GitHub numa pasta nova, sem MSYS2: o log mostrava `[hwopus] FFmpeg has no Opus decoder` logo antes; `config_components.h` do FFmpeg da release: `CONFIG_OPUS_DECODER 0` | o script de dependências herdado do LAS Recomp (`--disable-swresample`) desligava em silêncio o decodificador Opus, que depende do swresample; o Zelda não usa Opus, o Let's Go usa em toda a música e efeitos | swresample ligado e o script falha se faltar qualquer decodificador que o jogo usa (`tools/build_deps_windows.sh`); sem Opus o runtime para com uma mensagem clara (`runtime/services/hwopus.c`). Release 0.1.1. Lição: testar a release como um jogador, até passar da tela do controle, antes de publicar |
| 41 | Barra da montagem do jogo em 100% minutos antes do fim | captura da janela do launcher durante a compilação | o progresso do gerador somava 0,5 por módulo: fechava em 1 com os 2 módulos do Zelda, mas chegava a 2 com os 4 do Let's Go | cada módulo vale 1/4 (`runtime/gamebuild.c`) |
| 42 | Na release 0.1.1, jogo novo (sem save) fechava na tela de título (`exception c0000005 accessing 0x39`) | teste do visitante em duas fases (montagem pelo launcher; depois jogo novo com entrada roteirizada): `[movie] cannot open extracted/romfs/bin/movies/EEVEE_GO/title_male.webm` logo antes | o player de vídeo (HLE) abria os vídeos direto da pasta `extracted/romfs/`, que só existe no ambiente de desenvolvimento; a montagem da release cria só a imagem `romfs.bin`. Com save, o vídeo do título não toca, por isso o teste anterior passou | os vídeos são lidos de dentro do `romfs.bin` por um leitor próprio da FFmpeg sobre o índice do `fs.c` (`romfs_image_file`); ordem: arquivo de um mod, pasta `romfs/`, imagem (`runtime/hle/movie.c`, `runtime/services/fs.c`). Release 0.1.2 |
| 43 | A documentação afirmava que o jogo não tem passo de tempo global, e o 60 fps no mundo funcionava sem explicação | levantamento de todas as leituras da tabela `G` no `main` (`adrp`+`ldr`, 9 instruções), sonda do passo recebido (`LARECOMP_ELAPSED_LOG`) e contagem de quem lê cada passo (`LARECOMP_TIMER_USERS`) | a varredura antiga procurava floats perto de 1/30; o passo é um inteiro em ns num objeto de temporizadores na pilha da thread principal: o passo 1 vem de `G[1]` pelo accessor `f_7000039600` a cada atualização do mundo, o passo 0 é fixo. Só os sistemas que convertem tempo em quadros pelo passo 1 seguem o relógio; contadores de quadro (captura, batalha) não | PROGRESSO e PESQUISA-60FPS corrigidos ("Mapa temporal"); o modo 2 (contadores divididos) mostrou-se insuficiente para a batalha (`tools/teste60.sh`): batalha e captura ficam com lógica a 30 |
| 44 | 60 fps (modo 1): na captura e na batalha o quadro intermediário tremia, pulava e misturava a interface (títulos sobrepostos, painéis do menu fora do lugar, retângulos brancos) | pares intermediário/final gravados no mesmo tempo de cena (`LARECOMP_WINDOW_SHOTS_REL`), diagnóstico por buffer e motivo de rejeição (`LARECOMP_INTERP_DIAG`), métrica de pixels inventados (`tools/interp_metrics.py`) | três causas: (1) os ganchos que marcam o objeto de cada desenho só eram instalados se a interpolação estivesse ligada no boot, e no modo 1 ela só liga dentro das cenas: tudo era pareado pela ordem (fantasmas, interface misturada); (2) os desenhos de interface também caem dentro de objetos, então "tem objeto" não separava 3D de 2D; (3) a mistura era palavra a palavra, e uma linha de matriz com um componente misturado e outro mantido (cruzando o zero, ou um inteiro no lugar de um float) deforma o modelo; além disso o intervalo até a imagem final herdava os 16,7 ms do mundo no começo de cada cena | ganchos instalados sempre que o modo 60 está ligado (`cmdbuf.c`); só se misturam desenhos com objeto, alvo com profundidade e teste de profundidade ligado (interface fica com o quadro atual); mistura por vec4, tudo ou nada, aceitando 0 como float; período fixo de 33,3 ms no modo 60. Medido: batalha 42 de 47 quadros sem pixels inventados (os 5 restantes são cortes e movimentos grandes de câmera, coerentes na imagem), captura 0 de 46 |
| 45 | 60 fps (modo 1): na batalha, personagens esticados e Pokémon deformados no quadro intermediário (a captura já estava boa) | batalhas roteirizadas com pares intermediário/final a cada 4 quadros nos ataques (`tools/teste60.sh batalha 1`), métrica de pixels inventados e imagens ampliadas dos quadros apontados | (1) corte de câmera (Eevee → close da treinadora): os ossos da treinadora longe e de perto eram misturados (o limiar de "salto" aceitava mudanças de até ~100%); (2) a mistura por vec4 deixava parte de uma paleta de ossos em N e parte no meio; (3) sombras: o mapa de sombra era redesenhado com as matrizes misturadas, mas o chão (sem objeto do jogo) usava a matriz de luz de N e a sombra escorregava | antes do redesenho, cada objeto mede quanto seus buffers mudaram: acima de 20% das palavras com passo grande ele fica com a pose de N; se os rejeitados concentram a maior parte da mudança (corte), não há intermediário; um salto em qualquer palavra deixa o buffer inteiro em N; todo desenho 3D (alvo com profundidade e teste ligado) é misturado, com ou sem objeto (os sem objeto formam um grupo). Medido em duas batalhas: 1 de 201 quadros apontado, coerente na imagem (zoom rápido de câmera) |
| 46 | Mesmo com as correções 44 e 45, a interpolação nas batalhas e capturas seguiu ruim no teste em jogo | avaliação do jogador em ritmo real | as medições automáticas (pixels inventados) não captavam o que incomoda em movimento | interpolação retirada do modo 60 (`exec.c`: só com `LARECOMP_INTERP=1`, pesquisa); batalhas e capturas ficam a 30 sem quadro intermediário; mundo, menus e Pokédex seguem a 60 nativos |
| 47 | 60 fps (modo 1): menus e Pokédex acelerados | menu aberto com `X` a 30 e no modo 1, cada quadro gravado (`LARECOMP_SCREENSHOT_EVERY=1`): a bola do menu aparecia em 4 quadros de jogo em vez de 8 | desde o commit "menus a 60", todo quadro fora de batalha/captura ia a 60; o menu para o mundo e conta os próprios quadros | só vai a 60 o quadro em que a atualização do mundo (`f_70002f8bb0`) rodou; menus, Pokédex e outras telas que param o mundo ficam a 30 (medido: idêntico ao de 30 quadro a quadro) |
| 48 | Depois da 47, menus e Pokédex ficaram sem aceleração, mas travados a 30 (o jogador lembrava de uma versão em que estavam perfeitos a 60) | comparação com as versões testadas: a "perfeita" era a com quadro intermediário nessas telas | menus e Pokédex só ficam fluidos e na velocidade certa com a lógica a 30 e o quadro intermediário; a 60 nativo eles aceleram | interpolação ligada só em menus (`fps60_menu`: mundo parado, sem batalha ou captura), período fixo de 33,3 ms; batalha e captura seguem sem ela. Medido no menu aberto com `X`: intermediários sem pixels inventados |
| 49 | Pokédex travada a 30 mesmo com o quadro intermediário nos menus; na abertura o contador oscilava entre 30 e 60 | rolagem da lista com cada par intermediário/final gravado; diagnóstico de rejeição (`LARECOMP_INTERP_DIAG`) | (1) a Pokédex é 2D, e o 2D ficava fora da mistura; (2) o detector de corte da batalha rejeitava todo quadro de menu (quase toda mudança da tela é grande); (3) com o 2D ligado, linhas recicladas da lista cruzavam a tela no meio; (4) na abertura o intermediário ligava e desligava (telas sem mundo, carregamentos) | nos menus o 2D também é misturado, sem o detector de corte, com teste de salto estrito para 2D (mudança acima de 30%: o desenho fica em N); intermediário de menu só depois de o mundo rodar a primeira vez. Medido: rolagem da Pokédex 25 de 25 quadros interpolados sem pixels inventados; menu principal sem pixels inventados |
| 50 | Na mira da captura, com o analógico solto, a mira continuava andando; qualquer toque já movia | leitura do analógico em `runtime/hle/input.c` | o valor bruto do SDL ia direto para o jogo, sem zona morta (um controle parado lê algumas centenas, e o jogo trata qualquer valor como empurrão) | zona morta radial com reescala (`stick_deadzone`, padrão 0,15, ajustável no launcher em "Teclado e mouse": "Zona morta do analógico"), também no desvio simulado dos testes |
| 51 | 60 fps: piscada feia ao abrir o menu andando (parado quase não acontecia) | menu aberto duas vezes andando, pares intermediário/final gravados na segunda abertura | com a interpolação desligada (mundo), os dois quadros gravados da última vez que ela esteve ligada continuavam marcados como válidos: o primeiro intermediário do menu seguinte misturava a cena atual com a do menu anterior | os quadros gravados são descartados sempre que a interpolação desliga; medido: segunda abertura andando sem pixels inventados |
| 52 | Issue #1: Let's Go Pikachu 1.0.2 numa AMD com Win10 fechava no boot (`c0000005` em `0x38`, fonte), abortava nas configurações do GO Park e o launcher recusava o Pikachu; o patch enviado fazia o setup aceitar "qualquer versão" | leitura do patch e do log do usuário; busca de endereços fixos no runtime e no gerador; teste com o Pikachu 1.0.0 do próprio usuário (setup pelo launcher, jogo novo roteirizado até a tela do nome) | (1) a Yu Gothic Regular de alguns Win10 quebra o motor de fontes do jogo; (2) o `btm:u` não tinha o `AcquireBlePairingEvent` (cmd 21); (3) o setup conferia o build id do Eevee 1.0.0. Aceitar outra versão do jeito do patch quebraria a 0.2.0: o gerador troca 3 instruções em endereços do Eevee 1.0.0 (`tools/patches.py`, sempre) e os ganchos de 60 fps, batalhas selvagens e objetos são endereços desse `main` | setup aceita os dois jogos pelo program id (`main.npdm`); paridade (regra do projeto): os endereços do Eevee 1.0.0 foram achados no `main` do Pikachu 1.0.0 comparando instrução por instrução, com os campos relativos mascarados (`tools/addrmap.py`: deslocamentos de -0x30 a +0x10, offsets de struct idênticos, a tabela `G` e o slot `0x15fab08` no mesmo lugar); os ganchos continuam escritos com o endereço do Eevee e o `fnhook` traduz pelo build (`rt_fnhook_set_build`, `rt_addr`); o gerador move os 3 remendos (`BUILD_ADDRS`, `tools/patches.py`); outros builds (atualizações) ficam sem eles e o launcher desativa as duas opções; versão real informada ao jogo (`game_version`); espera ignora slot de handle vazio; dump plano; save do Pikachu em `save-pikachu/`; tradução pelo title id do jogo montado; Yu Gothic Medium e MS Gothic antes da Regular; cmd 21; menu sobreposto em EN/PT/RU; últimas chamadas ao host no relatório de crash e `lasrecomp-prev.log`. Release 0.2.1 |
| 53 | Com 60 fps e batalhas selvagens ligados (0.2.0, nos dois jogos), a batalha selvagem ganhava quadro intermediário e ficava "ridícula" (o jogador viu no Pikachu; o teste da 0.2.0 só conferiu que a batalha começava) | log da sessão do jogador: depois de `encounter intercepted` nenhum `scene start`; o mesmo no log do teste da 0.2.0 com o Eevee | a batalha selvagem é montada pelo processo das batalhas estáticas (`wildbattle.c`), que nunca chama `BATTLE_START`: sem `g_special`, o mundo parado contava como menu (`fps60_menu`) e ia para a interpolação | o modo batalhas selvagens avisa o 60 fps ao montar a batalha (`fps60_scene_begin`); o log do 60 fps separa os três estados (mundo / menu com intermediário / batalha e captura). Conferido por roteiro no Eevee (encostou na Rota 1: `battle / capture: 30 fps`) e jogando nos dois jogos. Lição: testar as opções juntas, não só cada uma |
| 54 | Issue #1 (segunda mensagem, que só vi depois de responder a primeira): troca local, Presente Misterioso e cooperativo pela internet fechavam o jogo | patch v2 do usuário; o `service_lookup` (`runtime/ipc.c`): serviço sem implementação vira um stub que responde ok sem devolver objeto | `nifm:u`, `ssl`, `bsd:u` e `ldn:u` não existiam: `CreateGeneralService` / `CreateUserLocalCommunicationService` voltavam sem sessão e o jogo usava um ponteiro nulo | `runtime/services/network.c`: console offline (nifm sem internet 2110-0300, eventos já sinalizados, socket com ENETDOWN, scan com 0 redes); os demais comandos do `btm:u` sem dados. Testado por roteiro nos dois jogos: troca local procura e o B volta, Presente Misterioso fica em "Connecting..." (o jogo não tem saída offline ali), GO Park procura e o B volta. Lição: ler a issue inteira antes de responder. Release 0.2.2 |

O que sobra numa primeira vez: um objeto nunca visto antes (Pokémon ou efeito novo) no meio da gameplay pode surgir 1-2
quadros depois, porque o driver leva ~30 ms para criar o pipeline e a espera por quadro é de 20 ms. Da segunda vez em
diante, o cache resolve.

Sombras: em 4K as bordas já saem suaves (o jogo filtra as sombras); um mapa maior que 2048² traria pouco e mexeria na
escala de alvos que não são 16:9 - fica de fora. Física e lógica rodam o código do próprio jogo, recompilado: o
comportamento é o do Switch.

---

## Auditoria de resolução dinâmica (17/10/2026)

| # | Sintoma | Diagnóstico | Causa | Correção | Arquivo |
|---|---|---|---|---|---|
| 55 | Resolução dinâmica podia descer mesmo com os 30 fps padrão estáveis | `tools/test_output_scale.c` executou a função real por 180 presents a 33,33 ms: escala 0,70 antes, 1,00 depois; a 45 ms, depois, escala 0,70 | `dyn_update` comparava o tempo entre presents com limiares fixos de 60 Hz (17,4/14,5 ms), enquanto o ritmo padrão é 30 Hz (~33,3 ms) | limiares proporcionais ao ritmo de 30/60 Hz e descarte da janela de amostras quando o ritmo muda; validado em jogo depois, com ini LF (o da auditoria era CRLF e não ligava o modo, ver 56): Eevee a 30 fps, código antigo 1,00 → 0,60, novo 0,95-1,00; modo 60 no mundo a 60,49 fps. Prioridade P3: a opção só existe no ini | `runtime/nvn/output_scale.c`, `tools/test_output_scale.c` |
| 56 | Valores do `larecomp.ini` editado à mão com CRLF eram ignorados em silêncio (`resolution = dynamic`, `upscaler = bilinear`, `75%`...) | ao validar a auditoria: o jogo recebia `res_mode 0` com `resolution = dynamic` no ini (log temporário em `output_scale`); mesmo forçado a 20 fps, nenhum `[dynres]` | o `fopen` lê em modo binário, então o CR ficava no fim do valor; só `mod` e `game_lang` eram limpos | toda linha perde CR/LF/espaços finais antes do `sscanf`; o mesmo ini CRLF a 20 fps agora reduz a escala (1,00 → 0,70) | `runtime/settings.c` |

---

## Locks do kernel (17/10/2026)

| # | Sintoma | Diagnóstico | Causa | Correção | Arquivo |
|---|---|---|---|---|---|
| 57 | O mundo da Rota 1 (~600 draws por quadro) rodava a só ~60 fps sem limite de fps, e a 60 fps pacado ficava em 58,8-59,3 fps (P99 ~19,6 ms), com nenhuma thread acima de ~41% de CPU (total 1,09 núcleo). O custo do backend Vulkan era pequeno (~3 ms por quadro; descriptor sets 0,45 ms) | `LARECOMP_VK_PROF` (CPU do backend e GPU por quadro), CPU por thread amostrada de fora, `LARECOMP_PROF_SVC`: 3 threads do jogo gastavam ~750 ms a cada 2 s em `ArbitrateLock` (~62 mil chamadas cada, ~12 µs por chamada). `LARECOMP_PROF_LOCKS`: um único mutex do jogo respondia por ~126 mil esperas em 2 s (`nn::os::LockMutex` chamado de `main+0x2e704`; o `main` não tem símbolos, o nome que o `addr2sym` mostra ali é só o mais próximo). Desmontagem do SDK (`InternalCriticalSectionImplByHorizon::Enter` e `Leave`): depois do svc o `Enter` relê a palavra do mutex, confere se é o dono e repete o laço, e o `Leave` com waiters só chama `ArbitrateUnlock` | Cada espera por mutex contendido dormia numa condvar do kernel (sob o `g_klock` global) e a posse era entregue a uma thread adormecida: ~12 µs de latência para acordar uma thread no Windows, cerca de mil vezes por quadro por thread | Antes de dormir, a thread espera girando até 40 µs no próprio estado de espera (`LARECOMP_LOCK_SPIN_US`; 0 volta ao comportamento antigo; desligado com menos de 4 processadores lógicos). A posse continua sendo entregue por `mutex_release`, então a semântica do kernel não mudou. `LARECOMP_PROF_LOCKS=1` lista os mutexes disputados a cada 2 s. Medido (Eevee, RTX 3080, janela 1280x720, mesmo binário, A/B intercalado): custo por espera do mutex quente 12 → 1,2-1,4 µs; esperas dele ~130 mil → 41-51 mil por 2 s; mundo sem limite de fps ~60 (59,1 e 61,0) → 73,7 e 65,1 fps; 60 fps pacado (Rota 1, 2 rodadas cada) 58,8 e 59,3 → 60,0 e 60,0 fps, P99 19,6 → 17,3 ms. Pewter City a 60 fps pacado: 60,0 fps nos dois jogos e já antes. Bateria (`tools/bateria.sh`) nos dois jogos, 3 rotas: 0 crash, 0 erro de validação, 0 chamada não implementada, piscadas equivalentes com e sem o spin. Limites: uma máquina e uma GPU; sem AMD/Intel, sem máquina de 2 núcleos (o spin fica desligado ali); jogo novo testado por 6.400 quadros com áudio só até a abertura. O gargalo seguinte do mundo é a GPU (~9-10 ms por quadro a 1280x720) mais o resto da cadeia entre as threads do jogo. A documentação antiga dizia ~160 fps sem limite no mundo; nesta rota medi ~60 antes e ~65-84 depois | `runtime/kernel.c` |

| 58 | Com os locks do kernel resolvidos (57), o mundo da Rota 1 ainda pesava ~9-10 ms de GPU por quadro numa RTX 3080 (84% num passe só), quase nada no recorte | `LARECOMP_GPUPROF_AT` com carimbo por draw e estatísticas de pipeline (`LARECOMP_GPUPROF_DRAWS`/`_STATS`); experimentos de um programa só (pular, scissor de 1x1, metade das instâncias, retorno no início do shader, só buscar atributos), sempre alternando na mesma execução; estatísticas do shader compilado pelo driver. Um grupo de 4 draws instanciados custava ~4,9 ms (um terço da GPU), só de vértice (0 fragmentos); o shader compilado é pequeno (48 registradores, sem spill) e um shader que só lê os atributos custava o mesmo que o completo; o custo sumia sem a busca. Várias hipóteses ficaram pelo caminho como ruído: a GPU varia 10-18 ms entre execuções do mesmo código | `guest_direct` ligava o heap importado do jogo (memória do sistema) como buffer de vértices e de índices, "no lugar" como no Switch: a GPU relia pelo PCIe os 8 streams de instância (16 bytes cada) para cada uma das ~71 mil invocações de vertex shader de um draw (~9 MB, ~0,8 ms a ~11 GB/s) | Anel de VRAM para vértices e índices em placas discretas: a CPU escreve num staging em memória do sistema e cada submissão começa copiando o que foi escrito (buffer de upload, com barreira); `LARECOMP_VERTEX_READ=direct\|copy\|auto`. O jogo usa compute para gerar dados de vértice (buffers de 16 bytes lidos como streams), então os storage buffers de compute (e de shaders de vértice ou de fragmento com `STG`) entram num conjunto de intervalos e essas faixas seguem sendo lidas no lugar; um shader que escreve em memória global sem storage buffer ligado desliga as cópias (aviso no log). Cópia por endereço inicial e buffer ligado inteiro (37,3 -> 16,2 MB por quadro); varredura de índices em AVX2. Medido (Eevee, RTX 3080, execuções intercaladas): GPU 10,2 -> 1,2 ms (-88%), fps sem limite no mundo 75,1 -> 89,6 (mediana; a CPU paga ~1,8 ms pelas cópias), 60 fps pacado inalterado (60,0 fps, P99 17,3-18,6 ms). Bateria nos dois jogos, 3 rotas, com o modo `auto`: 0 crash, 0 erro de validação, 0 chamada não implementada, 0 draws pulados, piscadas na mesma faixa; mundo do Eevee com as camadas de validação do Vulkan ligadas: 0 erros; quadros dentro do ruído entre execuções (PSNR ~33 dB contra ~34 dB do mesmo modo). Limites: uma máquina e uma GPU; placa integrada e GPU fraca sem teste (o modo `auto` mantém o comportamento antigo na integrada); a igualdade de imagem não é pixel a pixel (a cena não se repete). Pesquisa completa e o que foi descartado (write watch, shader achatado, pilhas em vetor): [PESQUISA-GPU.md](PESQUISA-GPU.md) | `runtime/nvn/vulkan/exec.c`, `maxwell/shader.h`, `maxwell/translate.c` |
| 59 | Com o mutex (57) e o PCIe (58) resolvidos, o próximo limite era a cadeia de CPU entre as 3 threads do jogo: o backend Vulkan rodava na thread do jogo e o gráfico (listas, apresentação, texturas) atrasava a lógica | A/B intercalado com `tools/perf_ab.py` (execuções em turbo alternadas, razão com intervalo de 95% por bootstrap) e bateria rápida paralela (`tools/bateria.sh` + `tools/bateria_scan.py`, ~100 s para 6 rotas) | O trabalho do backend (~3 ms por quadro) ficava na cadeia: sem ele a cadeia roda a ~159 fps | Thread de renderização (`runtime/nvn/vulkan/renderthread.c`): fila ordenada com tickets de fence, carimbo de páginas escritas por lista, destruição de textura e `backend_represent` por `rt_call`; contador `renderer_game_frame` (jogo) separado de `renderer_present_count` (backend). **Ligada por padrão** (`render_thread = 0` no `larecomp.ini` ou `LARECOMP_RENDER_THREAD=0` desliga; `vertex_read = auto\|copy\|direct` também no ini). O mapa quebrado depois de capturas que apareceu com ela era do pool de texturas (item 60). Medido (RTX 3080, execuções intercaladas, sem limite de fps): Eevee 126,9 -> 185,5 fps (razão 1,462, IC 1,40-1,53; P99 do tempo de quadro 13,3 -> 9,9 ms), Pikachu 132,1 -> 191,4 (1,449, IC 1,31-1,54); 60 fps pacado inalterado (16,63 ms nos dois modos). Bateria dos dois jogos, 3 rotas, 3 repetições por modo: diferenças de quadro dentro da variação entre execuções do mesmo modo, piscadas na mesma faixa, 0 erro de validação do Vulkan, 0 chamada não implementada, 0 crash; fechar a janela sai normal. Limites: uma máquina e uma GPU; combinação de depuração camadas de validação + thread trava na saída depois do `_exit` | `runtime/nvn/vulkan/renderthread.c`, `device.c`, `exec.c`, `textures.c`, `runtime/settings.c` |
| 60 | Na 0.3.0 (Eevee, `fps = 60`), depois de entrar e sair de uma captura o mundo podia ficar quebrado até trocar de área: cercas, placa e samambaias com cor chapada ou escuras, manchas cinza no chão | Variantes jogadas à mão com cópia do save: quebrou com a thread de renderização ou com a cópia de vértices, sem as duas não apareceu; intermitente. F5 (texturas reconferem a memória) e F6 (vértices no lugar) não consertam; `F9` nos estados bom e quebrado: mesmas contagens de vértices por shader, só o aspecto dos objetos muda. Então: textura errada, não dado velho. Lendo o caminho do pool de texturas: `nvnTexturePoolRegisterTexture` guardava o **ponteiro host** do objeto; com contadores no log (`LARECOMP_POOL_FIX=0`) e o loop de capturas automático (`tools/capture_loop.py`), centenas de slots ficavam em texturas finalizadas, e slots de texturas reinicializadas apontavam para outro objeto (16 já no boot); os religamentos acontecem justo depois de cada captura | O jogo finaliza uma textura e a inicializa de novo no mesmo objeto NVNtexture (streaming) sem registrar o slot outra vez. No console o descritor continua valendo; aqui o slot ficava no objeto host liberado e mostrava o que o `malloc` pusesse no lugar: certo por sorte quando o mesmo pedaço voltava, outra textura quando não. Qualquer mudança no padrão de alocação (thread de renderização, cópias de vértices) mudava a sorte, por isso cada opção "causava" o bug | `runtime/nvn/objects.c`: o slot guarda também o objeto do jogo (`NvnTexEntry.guest`); `nvnTextureFinalize` limpa os slots da textura e `nvnTextureInitialize` no mesmo objeto devolve o novo host a eles. Thread de renderização e vértices na VRAM voltam ligados por padrão. Verificado: loop de capturas com as duas opções ligadas, Eevee e Pikachu, mundo normal depois de cada encontro (pares antes/depois); bateria rápida dos dois jogos sem crash, sem erro de validação, piscadas na faixa de sempre. Ferramentas novas: `tools/capture_loop.py` (anda na grama, foge dos encontros: B, três vezes para baixo, A), `LARECOMP_INPUT_LOOP`, marcas `[scene]` no log | `runtime/nvn/objects.c`, `nvn.h`, `runtime/hle/input.c`, `runtime/hle/fps60.c`, `tools/capture_loop.py` |
| 61 | Revisão do LAS Recomp upstream (18/10/2026): o que mudou desde a cópia em `reference/LASRecomp` (81bf718, 14/10) e vale para o Let's Go, parte Windows | Clone do upstream em `D:\projects\upstream\LASRecomp`; o desenvolvimento está na branch `beta_5` (255 commits; o `main` só tem randomizer e README). Triagem commit a commit; descartados os do Zelda (randomizer, mini-chefes, ganchos `delegate_null`, patch da fonte), os que já temos (condvar no timeout, store em gmem nos draws, vértices na VRAM, CR/LF no ini) e os de Mac/Linux. Testes: pasta do jogo com nome em cirílico e português (junção para `extracted/`); memória comprometida medida de fora (Get-Process) na Cidade de Pewter; salvamento roteirizado pelo menu X → Save | (1) caminhos UTF-8 lidos pela code page do sistema: `????-ação/exefs/main` não abria; (2) winpthreads compromete o tamanho pedido da pilha: 256 MB por thread do jogo; (3) `IFile`/`IDirectory` nunca fechavam o arquivo do host; (4) depois do crash, a caixa "Visual C++ Runtime" do `abort()`; (5) controles que o SDL não reconhece não viravam gamepad | (1) manifesto com `activeCodePage` UTF-8 (bcf999a): a build nova abre a pasta em cirílico; (2) `_beginthreadex` com `STACK_SIZE_PARAM_IS_A_RESERVATION` (517a19f): memória privada 7,2-7,4 GB → 4,7 GB, working set igual; (3) `on_close` fecha o descritor (parte do 1c805c2); o save continua gravando igual à build anterior; (4) `_set_abort_behavior` (bc26a48); (5) `gamecontrollerdb.txt` na pasta de dados (8f28a0e). Também: relatório de crash nomeia `f_<endereço>` antes do nome do DLL (dc30ccc). Bateria rápida dos dois jogos sem crash nem erro, piscadas na faixa de sempre | `runtime/platform/windows.manifest`, `windows.rc`, `windows_threads.h`, `windows.c`, `runtime/kernel.c`, `runtime/services/fs.c`, `runtime/hle/input.c`, `runtime/dispatch.c` |
| 62 | Mesma revisão, parte Vulkan e tradutor de shaders | Leitura dos diffs contra o nosso `exec.c`/`translate.c`; bateria antes/depois com o executável antigo compilado à parte (`EXE=` novo em `tools/bateria.sh`) e quadros comparados: a renderização é a mesma, só muda o tempo (o menu do Eevee varia assim entre duas execuções da mesma build) | (1) `renderer_fence_seq` contava um slot reaproveitado que ainda tinha os flags da submissão anterior: um fence de fila vazia esperava um valor que nunca seria sinalizado; (2) slots de cor vazios no fim do passe iam como `VK_NULL_HANDLE` (o driver Intel UHD 630 trava nisso); (3) a chave de pipeline copiava os 8 formatos de cor, inclusive os de passes anteriores: pipelines duplicados; (4) IPA multiply supunha o multiplicador = w; (5) I2I .SAT, SHF e HSETP2 .H_AND fora da semântica do hardware | (1) 37b7ff6; (2) f4cb039 (um slot vazio no meio vai para o log); (3) 3921201; (4) d1c2d8e; (5) d4c2ff6, mas SHF .CC/.X/.HI continuam ignorados em vez de derrubar o programa | `runtime/nvn/vulkan/exec.c`, `runtime/nvn/maxwell/translate.c` |
| 63 | Mesma revisão, parte desempenho: o que o upstream mediu no Zelda vale aqui? | GPU por quadro (`LARECOMP_VK_PROF`) na rota do mundo em escala 2x, duas execuções de cada, intercaladas; `tools/perf_ab.py` (5 + 5 execuções intercaladas, nos dois jogos) com uma variante `EXE=` apontando para o exe e a DLL anteriores; bateria e quadros comparados | (1) a conversão half → float bit a bit (~12 operações e dois desvios por half) rodava em todos os estágios; (2) a libc do SDK (memcpy, memcmp, strlen...) recompilada byte a byte e o `fesetround` do caminho lento de ponto flutuante (que mexe também no x87) custavam ~3% de CPU no Zelda | (1) **portado** (50a09ed): fragment e compute usam `unpackHalf2x16`, vertex continua exato (`LARECOMP_VK_EXACT_H2F=1` volta ao antigo). RTX 3080: GPU 4,01-4,06 → 3,70-3,81 ms por quadro; os quadros só diferem na folhagem com vento, como duas execuções da mesma build. (2) **testado e não portado** (67bad06, a2b4aa3): DLLs refeitas para os dois jogos, bateria limpa, mas o A/B deu razão 1,008 [0,989; 1,017] no Eevee e 1,001 [0,987; 1,012] no Pikachu (turbo ~180 fps): sem ganho mensurável nesta máquina, e mudar o `GAMELIB_ID` obriga cada jogador a recompilar o código do jogo ao atualizar. Fica a dependência certa no Makefile (objetos do jogo recompilam quando `simd.h`/`simd_fp.h` mudam). Na bateria final, uma vez a rota de menus do Pikachu abriu a Bolsa em vez da Pokédex (tempo do roteiro com 6 rotas em paralelo); repetida 3 vezes, igual à build antiga | `runtime/nvn/maxwell/translate.c`, `Makefile`, `tools/perf_ab.py` |
| 64 | Na 0.3.1, o launcher salvava o `larecomp.ini` com linhas grudadas (`fullscreen = 0fps = 60render_height = 1440smaa = 1mod = ...`): na leitura seguinte voltavam ao padrão fps, resolução, SMAA, tradução, áudio dos vídeos, contador de fps, d-pad e batalhas selvagens | Achado ao ler o `settings.c` para a opção de vibração; reproduzido com o launcher em autoplay salvando num ini de teste (`LARECOMP_SETTINGS`): a build antiga gravou as linhas grudadas, a nova não | O commit 8a2e713 (gerado por script) perdeu o `\n` de 9 `fprintf` do `settings_save`: o canal de comandos converte `\n` em quebra de linha real (a mesma armadilha do CLAUDE.md) | `\n` de volta; a leitura separa uma chave grudada no valor anterior (letras minúsculas e `_` logo antes de um segundo ` = `), então um ini salvo pela 0.3.1 recupera tudo: testado com um ini estragado, o launcher regravou fps 60, 1440, SMAA, tradução, d-pad e batalhas selvagens | `runtime/settings.c` |
| 65 | Risco de crash `c0000005` num endereço válido do guest quando duas threads tocam ao mesmo tempo um bloco de 64 KB ainda não comprometido (não visto no Let's Go; apareceu no port do Sword, que usa o mesmo runtime, em 11 s de um teste) | Leitura do `lazy_commit`: ele só aceitava página em `MEM_RESERVE`. A primeira thread compromete o bloco; a segunda, que já tinha falhado antes disso, encontra `MEM_COMMIT`, o handler devolve falso e a exceção vira crash. (As pilhas das threads do host, só reservadas desde o 290e848, ficam fora da janela do guest: não passam por aqui) | Corrida entre falhas simultâneas no mesmo bloco | Página já `MEM_COMMIT` e `PAGE_READWRITE`: repetir o acesso; as protegidas de propósito (`plat_protect`, `LARECOMP_WATCH`) continuam falhando, e falhas de execução (DEP) não passam pelo `lazy_commit` (não entram em laço). Bateria dos dois jogos sem crash nem erro, piscadas na faixa de sempre | `runtime/platform/windows.c` |
| 66 | Na 0.3.2 (30 fps), depois de sair de uma captura: árvores e plantas pretas com listras amarelas, moitas em X com a textura de uma placa ou em cor chapada azul-clara, o chão sem a trilha de terra, até trocar de área. O jogador via quase sempre; os roteiros automáticos, quase nunca | Detectores no jogo do jogador, com o bug na tela, todos em zero: slot do pool vazio ou com outra textura, memória do pool mexida por fora, cópia buffer→textura só na GPU, imagem diferente da memória, registro por objeto copiado ou finalizado (o item 60 tinha tratado o pool; não era isso). O despejo do F9 com o slot de cada textura mostrou o chão de moitas com um shader de fragmento de 5 texturas no lugar do de 4, a textura 0 herdada do draw anterior: shader errado, não textura errada | O cache de programas traduzidos (e o de compute) usava só o endereço do código como chave. Depois de uma captura o jogo carrega shaders de novo em endereços que antes tinham outros; o renderer devolvia a tradução antiga (outro layout de texturas, outra iluminação) | A chave inclui um hash do código e das constantes do shader, calculado no `SetShaders` (`content_hash`); a sondagem da tabela de programas é limitada (64 posições). O log conta os endereços que passam a ter outro shader (`[shader] ... now holds another shader`): em loop de capturas (`LARECOMP_INPUT_LOOP_CATCH=1`), 7 capturas no Eevee e 9 no Pikachu, sem crash, o contador aparece nos dois logo depois das capturas. Ferramentas novas: o loop de capturas que captura de verdade, `--leg` e `EXE=` no `tools/capture_loop.py` | `runtime/nvn/objects.c`, `nvn.h`, `vulkan/exec.c` |

---

## Lições aprendidas
- **Medir antes de chutar.** Os maiores avanços vieram de instrumentação: dump de threads, motivo dos draws
  pulados, gravação do áudio, log de quem chama cada função.
- **O LASRecomp é "certo para o Zelda".** Vários bugs eram suposições que valiam para o Zelda e não para o Let's Go:
  formato de texto, Pro Controller, programas por estágio, buffer de vídeo do jogo, código de fim de stream.
- **A biblioteca do próprio jogo é a referência.** Ler o disassembly do SDK e do `subsdk1` (por exemplo
  `TimedWait`, `AndroidErrorToNnError`) deu a resposta exata em vez de tentativa e erro.
- **Corridas de thread aparecem como sintomas aleatórios.** O vídeo travava num quadro diferente a cada execução;
  a causa era uma corrida no kernel, não o vídeo.
- **Um teste que passa precisa exercitar o caso que falha.** O shader na CPU "provou" que a tradução estava certa
  com vértices que não passavam pela instrução defeituosa. Rodar com os dados exatos de um draw quebrado (exportados
  do RenderDoc) e comparar com a saída da GPU resolveu em minutos.
- **Tradução de registradores: cuidado com destino que também é fonte.** Instruções que escrevem vários
  registradores (`LDC.64`, `.128`) podem ter o endereço entre os destinos; leia as fontes antes de escrever.

## Giroscópio real do controle (16/10/2026, não validado em hardware)
Ideia tirada do `hid_motion.c` do ScarletNX (reimplementada, sem copiar código): o controle com sensor (DualSense, Pro
Controller) vira a mira como o mouse. `runtime/hle/input.c` liga `SDL_SENSOR_GYRO` ao abrir o controle e integra cada
`SDL_EVENT_GAMEPAD_SENSOR_UPDATE` (rad/s × tempo entre amostras) em yaw/pitch do giroscópio virtual, só enquanto o jogo
mira; zona morta de 0,015 rad/s contra o viés em repouso, amostras com intervalo > 100 ms descartadas. Opções:
`pad_gyro_sens` (launcher) e `LARECOMP_GYRO_PAD_FLIP`. Compila (`input.o`, `settings.o`); sentido dos eixos por conferir.

## Modo "batalhas selvagens" (17/10/2026, experimental)
Ideia: ao encostar num Pokémon selvagem, em vez da captura estilo Pokémon GO, uma batalha clássica e, depois, a captura,
como o jogo já faz com o Snorlax, as aves lendárias e o Mewtwo. Detalhes e endereços em [MODO-BATALHA.md](MODO-BATALHA.md);
código em `runtime/hle/wildbattle.c`; opção "Batalhas selvagens (experimental)" no launcher (`wild_battle = 1` no
`larecomp.ini`) ou `LARECOMP_WILDBATTLE=1`.
- **Como funciona.** As batalhas "estáticas" são o processo da classe A (fábrica `17f0a0`), que lê o registro de
  `bin/script_event_data/event_encount.bin` pelo hash em `[obj+0x98]`. O gancho no gatilho do toque (`305ad0`, substituído
  inteiro: a função original configura depois o handle da cena de captura, que um processo de batalha não aceita) cria
  esse processo; o gancho no construtor grava um hash nosso; o gancho na busca (`2bad00`) responde com uma cópia do
  registro do Snorlax com espécie, forma, nível, gênero e IVs do Pokémon tocado (lidos do objeto `P` do construtor da
  configuração: nível = byte `P+0xf0c`, IVs = `P+0xf18..0xf2c`, gênero = `P+0x40c`) e os golpes por nível da
  `waza_oboe.gfpak`. Depois da batalha, a captura do mesmo Pokémon é o fluxo nativo.
- **Sintoma: tela preta ao voltar ao mundo.** Diagnóstico: o mundo era desenhado (passes grandes) e só os passes finais
  saíam pretos; `LARECOMP_DEBUG_FRAME`/`g_debug_frame_request` e o log das chamadas de fade (`11ece0`, `11ee50`) mostraram um
  fade out tipo 1 ao fim da captura e **nenhum fade in** até a próxima captura começar sozinha. Causa: o script do evento
  (`FadeOut_` -> `CallWildBattle` -> `GetWildBattleResult_` -> `FadeIn_`) faz o fade in; nós pulamos o script. Correção: o
  gancho no `2f78d0` (o mundo, todo quadro) repete 30 quadros depois da batalha um fade in do tipo do último fade out.
- **Sintoma: batalha começava com flags trocadas.** `CallWildBattle` grava `a0 = a1 = 0, a2 = 1` e o construtor deixa
  `a0 = a1 = 1, a2 = 0`; copiamos os valores do script.
- **Sintoma: queda ao copiar a "configuração real".** Duas tentativas de lançar a configuração (0x2b8 bytes) do Pokémon
  por cima da da batalha derrubaram o jogo: o objeto de `x0` no gatilho tem 0xb8 bytes (não é a configuração) e a que o
  construtor devolve ainda estava com a espécie zerada. Desligado (`LARECOMP_WILDBATTLE_USECFG`, padrão 0); o caminho que
  funciona é ler os campos de `P` e montar o registro.
- **Lições.** (1) Procure primeiro o que a comunidade (pkNX) e o próprio jogo já dizem: os nomes dos nativos de script
  estavam em strings do `main`. (2) Roteirizar o teste (`LARECOMP_INPUT`) e salvar imagens a cada N quadros mostrou em
  minutos o que três rodadas manuais não mostraram. (3) O `LARECOMP_HOOK_DUMP` exige o endereço completo do guest.
- **Falta:** XP dividido (metade ao derrotar, metade ao capturar), natureza e shiny do Pokémon tocado, formas
  regionais (a tabela de golpes usa o índice da espécie), batalha de Pokémon grandes/auras e teste em mais áreas.

## Texto em 2x com riscos verticais (19/10/2026, PR #13 do Vancete)
- **Sintoma.** Com o cache de glifos em 2x (`runtime/hle/font_hires.c`, ligado sozinho a partir de 1080 linhas), o menu
  "Continue your adventure" mostrava riscos claros e finos entre as letras; com `LARECOMP_FONT_SCALE=1` não havia nada.
- **Diagnóstico.** Saturar a subtração do preenchimento (fontes com contorno) não mudou nada. Os riscos ficavam nas
  bordas das células: a célula em Kx era escrita de `cx*K + 1` até `(cx+cw+2)*K - 1`, deixando a primeira e a última
  coluna como estavam.
- **Causa.** Quando o jogo reaproveita uma célula, essas colunas guardavam pedaços do glifo que estava ali antes. Em 1x
  o jogo reescreve a célula inteira (as colunas vazias também).
- **Correção.** A célula em Kx cobre exatamente K vezes a de 1x (`cl = cx*K`, `cr = (cx+cw+2)*K`). Conferido em tela
  cheia 1080p (2x automático) no Eevee: sem riscos.

## Interface 2x com ícones em lixo no Pikachu (19/10/2026)
- **Sintoma.** Teste de paridade do PR #13: no Pikachu, com `ui2x = 1`, os ícones do menu X saíam como ruído e o ícone
  do parceiro sumia; com `ui2x = 0`, normal. No Eevee a mesma comparação saía certa.
- **Diagnóstico.** O Pikachu com o pacote gerado no Eevee ficava normal: o defeito estava no pacote. As interfaces dos
  dois jogos têm as mesmas 3081 texturas (mesmas chaves), mas os dados das 3081 diferiam. Gerar de novo no Eevee dava
  um pacote idêntico (é determinístico), e numa pasta de teste ligada aos arquivos do Eevee também. Gerar de novo no
  Pikachu deu o pacote certo, idêntico ao do Eevee. Ou seja, foi uma falha isolada: naquela vez o waifu2x terminou em
  37 s em vez de ~72 s, saiu com código 0 e entregou lixo em todas as imagens (1 em 7 gerações, RTX 3080).
- **Causa.** O empacotamento (`runtime/ui2x.c`, `pack`) aceitava qualquer saída do waifu2x com o tamanho certo.
- **Correção.** Cada imagem 2x, reduzida a 1x pela média de 2x2, é comparada com a entrada do waifu2x (cor e alfa).
  Diferença média acima de 16 (0..255): a imagem fica de fora. Mais de 10% de fora: não sai pacote, e a tela inicial
  diz que o ampliador falhou e para tentar de novo. Medido: imagens boas até 7,2; o lixo da falha entre 66 e 188.
  Três gerações seguidas com a conferência: 0 imagens de fora.

## Batalha selvagem quebrava no começo do jogo (19/10/2026, issue #6)
- **Sintoma.** Com "Batalhas selvagens" ligado, o jogo fechava com `exception c0000005 accessing 0x78` logo depois de
  "A wild ... appeared!", antes do "Go on, ...!". Relato no Pikachu (Rattata nível 3, Rota 1); o jogador disse que um mod
  do Gamebanana dava o mesmo.
- **Diagnóstico.** O loop de capturas (`tools/capture_loop.py`, `wild_battle=1`) com um save da Rota 1 (0:11 de jogo, só
  Eevee parceiro e um Pidgey) reproduziu nos dois jogos; com saves mais avançados, na mesma Rota 1, não. O crash é em
  `f_7000a9ddb0` (chamada pelo comando de sequência de batalha `f_7000aeebb0`): ela procura um componente do Pokémon que
  anda junto, não acha e passa um ponteiro nulo adiante. Tratar esse nulo (e o do setter `f_7000155870(0, 0)` que o
  próprio código chama nesse caminho, e que tem armadilhas do compilador como escrita em `0x130`) só levou o crash para
  outro comando (`f_7000adc4c0` -> `f_7000a9a8e0`): a abertura da batalha estática foi feita contando com o acompanhante.
  As flags do objeto da batalha (`a3..a6`) não mudam isso. Trocar a ordem da equipe no menu (Pidgey no slot 0) fez a
  batalha correr normal. Os nomes dos nativos de script (`PokePartyIsPartner`, `GetPartnerIndex_` = `f_700019a040`) deram
  a função que diz o slot do parceiro, `f_70002462e0(0, &slot)`. No save, o cabeçalho da equipe fica em `+0x5A00`
  (6 índices e, em `+0x0C`, o do parceiro) e os Pokémon em `+0x5C00 + índice x 0x104`; a cópia viva do jogo é outra.
- **Causa.** Quando o parceiro está no primeiro slot nenhum Pokémon anda junto (no começo do jogo isso nem pode ser
  mudado), e a abertura da batalha estática não prevê esse caso.
- **Correção (proteção).** No toque (`trigger_hook`), se o parceiro está no slot 0, o modo selvagem não monta a batalha
  e o jogo segue com a captura dele. Testado nos dois jogos: save da Rota 1, 3 encontros em cada, todos na captura e
  sem crash; com o Pidgey no slot 0, a batalha continua. **Falta:** batalha também sem acompanhante (a correção
  completa).

## Batalha selvagem: o inicial andando sozinho (19/10/2026, issue #6, 0.4.2)
- **Dois relatos depois da 0.4.1.** O usuário, com o Eevee na frente e o Bulbasaur andando junto, caiu na captura sem
  precisar: o caso "com acompanhante" tinha sido testado só com o Pidgey movido para o slot 0, e as rodadas com o save
  dele não acharam encontro (não conferi). O Vaskiemaia, com o Oddish na frente, teve batalha, mas o jogo fechou ao
  trocar para o Pikachu (vídeo; log com o mesmo `f_7000a9ddc0`, Pikachu).
- **Vocabulário do jogo.** Nos nativos de script, "partner" é o Pokémon que anda com o jogador e "buddy" é o inicial
  (`f_70001760d0`: Pikachu forma 8 ou Eevee forma 1). `f_70002462e0(0, &slot)` (o `GetPartnerIndex_`) dá o slot de
  quem anda junto, não do inicial. No save, o cabeçalho da equipe (`+0x5A00`) tem quem anda junto no slot 0; os
  blocos do save são conferidos por CRC16-ARC (tabela `FEEB` no fim: tamanho, id, checksum).
- **A causa.** Em batalha simples, `f_7000a88780` (abertura) e `f_7000a8ced0` (troca) mandam o inicial com
  `ee422.bseq` (o método virtual `f_7000a7e090` pergunta se é o inicial); os outros vão com `ee420.bseq` (Poké Ball). A
  `ee422` leva para a batalha o modelo de campo de quem anda junto; com o inicial andando sozinho, a batalha estática
  não tem esse modelo e `f_7000a9ddb0` lê um nulo. Por isso quebra também na troca.
- **O que não resolveu.** Os scripts das batalhas estáticas do jogo (`bin/archive/script_amx.gfpak`, AMX de 64 bits em
  codificação compacta; nomes de nativos pelo hash `h = h * 0x83 ^ byte`, achado no registrador `f_70006705e0`)
  recolhem quem anda junto e o inicial montado antes do `CallWildBattle` (`StartFollowPartnerIntoTheBall_` =
  `f_7000243bb0`, `StartRidePartnerIntoTheBall_` = `f_7000243d90`). Chamar isso antes da batalha, com ou sem espera,
  apagar os dois (`f_7000244440`, `f_7000244620`), fazê-lo reaparecer montado (`f_7000244270`, recusa), outro molde
  (Electrode) e forçar a `ee420` (ela tem o ramo do inicial no ombro e quebra em `f_7000adc1f0` -> `f_7000a9bf20`)
  não resolveram.
- **Correção (proteção, 0.4.2).** No toque (`starter_walks_alone`), se quem anda junto é o próprio inicial (ou ninguém),
  o encontro vai para a captura do jogo; com outro Pokémon andando junto, batalha, e o inicial pode entrar. A regra da
  0.4.1 ("inicial no slot 0") sai.
- **Ferramentas.** `LARECOMP_SCAN_LEN` / `LARECOMP_SCAN_NOPTR` na varredura de memória (docs/DEPURACAO.md).
  **Lição:** um caso só conta como testado quando o log mostra o evento dele; testar com cópia do save do usuário.

## SSAO e GI piscando com a câmera em movimento (19/10/2026, PR #16 do Vancete)

- **Sintoma.** Com a oclusão de ambiente (SSAO) ou a luz indireta (GI) ligadas, partes da cena (pedras, arbustos,
  personagens) ganhavam e perdiam o efeito enquanto a câmera se mexia, como se ele ligasse e desligasse. Visto pelo
  usuário na batalha e andando no ginásio de Pewter; já existia antes do PR.
- **Diagnóstico.** Em partidas automáticas o SSAO rodava em todo quadro, com histórico temporal mantido, e quadros
  seguidos não destoavam. Um dump da oclusão na batalha (`LARECOMP_SSAO_DUMP`) só tinha as árvores do fundo. Contando
  os desenhos opacos antes e depois do SSAO em cada quadro: na batalha, 30 antes e 29-30 depois; andando no ginásio de
  Pewter, de 34 a 68 antes (de cerca de 130), mudando de um quadro para outro.
- **Causa.** O efeito era aplicado no primeiro desenho translúcido depois de 16 opacos. O Let's Go desenha translúcidos
  no meio dos opacos, então o que vinha depois ficava sem oclusão e sem luz indireta, e a fronteira andava com a
  câmera (culling e ordem dos desenhos).
- **Correção.** O SSAO e a GI rodam quando o passe da cena termina, com todos os opacos desenhados
  (`ssao_draw_hook`, exec.c). Pewter: 118-128 opacos antes, nenhum depois, em todos os quadros.
- **Segunda causa (batalhas e capturas).** Depois da primeira correção, ainda piscava na batalha, mais visível na
  cena da treinadora derrotada. Um build com diagnóstico (o usuário reproduziu, o log contou) mostrou o alvo da cena
  estável, nenhum desenho fora e o histórico mantido, mas a câmera do quadro mudando: a cada poucos quadros, o slot de
  constantes dos desenhos da cena (vertex cbuf 5, +64) tinha uma câmera na origem (view sem translação, espelhada,
  near 16 e sem far: a do céu), em todos os desenhos daquele quadro, e a da cena não estava em slot nenhum como
  projeção própria. O SSAO e a GI liam a profundidade com essa câmera nesses quadros. A máscara de constantes do
  shader não ajudou (não marca esse slot nem nos quadros bons).
- **Correção.** Uma câmera na origem não é a da cena (`cam_at_origin`); sem câmera no quadro, vale a do quadro anterior
  se tiver até 8 quadros, parada (`ssao_settle_camera`, no fim do passe). Três batalhas: 197 a 554 quadros com a câmera
  na origem, todos menos 1 a 9 (começo de cena) cobertos.
- **Efeito colateral corrigido.** Aplicado no fim do passe, o efeito pegava também o fade que o jogo desenha por cima
  da cena no mesmo alvo: na abertura (fade a partir do branco), a grama aparecia cinza através do branco. Agora ele
  entra antes do primeiro desenho sem teste de profundidade depois dos opacos (um fade, as passadas de tela cheia), ou
  no fim do passe; os translúcidos 3D no meio da cena testam profundidade e não disparam. Pewter continua com nenhum
  opaco depois do efeito.
- **Não era deste problema.** As faixas horizontais nos quadros escuros da troca de mapa vêm do pacote da interface 2x
  (somem com `ui2x = 0`, com ou sem SSAO/GI; existem desde a 0.4.0).
- **Ferramenta.** `LARECOMP_SET_AT` aplica opções no meio da partida, como o F12 (docs/DEPURACAO.md).
- **Ainda aberto.** Trocar o antisserrilhado para TAA com o jogo rodando deixa 1-2 quadros pretos: os shaders da cena
  ganham variantes com jitter e os desenhos são pulados enquanto compilam (já acontecia na 0.4.x).

## Launcher abrindo em tela cheia (21/10/2026, testes dos PRs #17 e #19)

- **Sintoma.** Com `fullscreen = 1` no `larecomp.ini` (a pasta de teste do Pikachu), o launcher abria ocupando a tela
  inteira, e não na janela estreita e mais alta que larga que o Eevee mostrava. Captura do launcher: 1920x1080 em vez
  de 762x887.
- **Causa.** O launcher chama `menu_apply_window_requests` a cada quadro (para a entrada de texto), e essa função, na
  primeira chamada, aplicava `SDL_SetWindowFullscreen(g_settings.fullscreen)`: logo depois de `window_as_launcher`
  sair da tela cheia e dar o tamanho do launcher, ela voltava para a tela cheia. Herança do fork, de antes de
  `ui_window_init` e `window_as_game` aplicarem a opção. Já existia na 0.5.0, não veio dos PRs.
- **Correção.** `menu_apply_window_requests` só aplica a tela cheia pedida por F11 / Alt+Enter. Pikachu com
  `fullscreen = 1`: launcher em 762x887, e o jogo em 1920x1080 depois de Jogar (`LARECOMP_LAUNCHER_AUTOPLAY` +
  `LARECOMP_WINDOW_SHOT`).

## Viridian City lenta: cópias de buffer esperando a GPU (22/10/2026, issue #20)

- **Sintoma.** Queda de FPS em Viridian City (issue #20, RX 7600 + i5-10400F). Medido aqui (0.5.0, i5-14600KF + RTX 3080,
  1080p, sem limite, `LARECOMP_VK_PROF`): Viridian ~110-155 fps, a thread de renderização ocupada 89-93% do tempo
  (~8 ms por quadro) e a GPU só ~1,5-3 ms. A cena mais pesada para a CPU do backend, mais que Pewter e a Floresta.
- **Diagnóstico.** O perfil ganhou o tempo de CPU por tipo de comando (`ops:` na linha do `LARECOMP_VK_PROF`). Quase um
  terço da thread era o comando 43, `CopyBufferToBuffer`: 2 por quadro, ~1,2 ms cada. `LARECOMP_LOG_B2B`: cópias de 112 e
  32 bytes (constantes), nenhuma em memória que a GPU escreve.
- **Causa.** A cópia chamava `renderer_flush()` antes do `memmove`: enviava o trabalho e esperava a GPU terminar tudo. Só
  é preciso quando a GPU escreve a origem ou o destino, ou lê o destino direto da memória; os draws gravados antes
  levaram cópias das constantes e dos vértices.
- **Correção.** Espera só nesses casos: intervalos que a GPU escreve (`gpuw`), os storage buffers de shaders com memória
  global (novo `gpur`, lido no lugar) e vértices lidos no lugar (anel de VRAM desligado). Depois do `memmove`,
  `guest_copy_flush` (como no `UpdateUniformBuffer`). `LARECOMP_VK_COPY_WAIT=1` volta ao antigo. Viridian, A/B
  intercalado no mesmo exe (4 + 4 rodadas por cena): Eevee parado 143 -> 180 fps, andando 146 -> 151, Pikachu andando
  122 -> 143 (razão 1,175, IC 95% 1,08-1,23); o novo ganhou nas 12 duplas. Thread de renderização ~8,3 -> ~5,6 ms por
  quadro. Bateria dos dois jogos, novo contra antigo: 0 crash, piscadas na faixa de sempre.
- **Ainda aberto.** O mesmo exe varia muito entre rodadas nesta máquina (120-175 fps; núcleos de eficiência do 14600KF,
  não confirmado). A cópia de vértices (~12-19 MB por quadro em Viridian) é o próximo custo da thread.

## Viridian City: as cópias de vértices saem da thread de renderização (22/10/2026, issue #20)

- **Diagnóstico.** Depois da correção acima, a maior fatia da thread de renderização em Viridian eram as cópias de
  vértices e índices para o anel de VRAM: ~12 MB e ~1,1 ms por quadro (~10,6 GB/s); a varredura dos índices, ~0,05 ms.
  `LARECOMP_VCOPY_SAME`: 85% dos bytes copiados iguais à cópia anterior, mas conferir com um hash custou o mesmo que
  copiar; reaproveitar entre quadros pediria o rastreio de escrita do Windows na reserva do jogo (fica para depois).
- **Correção.** A thread de renderização reserva o espaço no anel e grava o draw; duas threads auxiliares fazem as
  cópias de 32 KB ou mais (em pedaços de 256 KB; a thread de renderização ajuda quando espera). Tudo termina antes do
  envio que lê as cópias e antes de a própria thread escrever na memória do jogo no meio da lista (`UpdateUniformBuffer`,
  cópia de buffer, cópia de textura para buffer, contadores). O jogo só reescreve vértices depois do fence do quadro,
  que vem depois do envio. `LARECOMP_VK_ASYNC_COPY=0` volta ao antigo.
- **Medido.** Viridian, CPU da thread de renderização ~5,6 -> ~3,6 ms por quadro (as auxiliares terminam antes do envio:
  0 ms de espera). A/B intercalado, mesmo exe, 4 + 4 rodadas: Eevee parado 188 -> 196 fps (razão 1,044, IC 95%
  1,027-1,057), andando 172 -> 181 (1,054; 1,037-1,063), Pikachu andando 171 -> 181 (1,061; 1,020-1,070); o novo ganhou
  nas 12 duplas, p99 menor. A thread de renderização deixou de ser o gargalo em Viridian: o resto é das threads do jogo.
  Bateria dos dois jogos, 2 repetições, com e sem: 0 crash, piscadas iguais.
- **Nota de teste.** No Git Bash, pôr `/c/msys64/usr/bin` na frente do `PATH` faz scripts rodarem no bash do MSYS2, e as
  variáveis exportadas não chegam (`TAG`, `REPS` da bateria, `LARECOMP_*`): não trocar o `PATH` antes de chamar scripts.

## Viridian City: girar antes de dormir nas esperas de condição - testado e não adotado (22/10/2026, issue #20)

- **Contexto.** Depois das duas correções do backend, Viridian fica limitada pelas três threads do jogo: cada uma passa
  ~45-55% do tempo esperando as outras em `WaitProcessWideKeyAtomic`; o resto é quase todo código do jogo recompilado,
  sem função dominante (amostragem do ponteiro de instrução das threads, ~500 Hz; o runtime é ~5%).
- **Medido** (`LARECOMP_PROF_CONDVAR`, novo): ~8.000 acordadas a cada 2 s, latência média de ~8-9 µs (~70 ms somados),
  quase metade das esperas abaixo de 50 µs.
- **Experimento.** Girar até 40 µs no próprio estado de espera antes de dormir (como os mutexes, HISTORICO 57). A
  latência somada caiu para ~45 ms, mas o A/B intercalado (4 + 4 rodadas por cena) não mostrou ganho: Eevee parado
  202 x 201 fps, andando ~187 x ~187, Pikachu andando ~185 x ~183. Removido; fica o medidor.
- **Visto no A/B.** Rodadas lentas (~123-135 fps contra ~185-205) aparecem nas duas variantes, ao acaso: o mesmo exe é
  bimodal nesta máquina (i5-14600KF, núcleos de desempenho e de eficiência). Hipótese a testar: as threads do jogo
  caindo em núcleos de eficiência.

## Rodadas lentas nas medições: a janela composta pelo Windows, não os núcleos de eficiência (22/10/2026)

- **Sintoma.** Nas medições de Viridian (sem limite de fps), o mesmo exe saía bimodal: ~185-205 fps ou ~115-140, ao acaso.
- **Hipótese descartada.** Núcleos de eficiência do i5-14600KF: com o processo preso aos núcleos de desempenho (lógicos
  0-11), e até a um lógico por núcleo de desempenho, as rodadas lentas continuaram; só nos de eficiência, 94-103 fps.
- **Causa.** A janela do jogo: `nvidia-smi` e a janela em primeiro plano lidos a cada 0,5 s. Clock da GPU igual
  (1875 MHz, P0) em todas; rodadas rápidas com a janela em foco e cobrindo a tela (carga da GPU ~50%), lentas com ela
  atrás de outras ou menor que a tela (65-77%). O Windows compõe essas janelas (DWM) em vez de mostrá-las direto:
  1920x1080 em foco 182-188 fps, 1280x720 em foco 151-158 (monitor de 300 Hz: não é o teto da tela).
- **Para quem joga.** Com limite, sem efeito medido: Viridian a 60 fps com a janela composta deu 60,0 fps, p99 ~17,4 ms.
- **Correção (medição).** `tools/perf_ab.py` traz a janela para a frente (sempre no topo, com foco) em cada rodada.

## Viridian City: os vértices ficam na VRAM de um quadro para o outro (22/10/2026, issue #20)

- **Diagnóstico.** 85% dos bytes de vértice copiados para a VRAM a cada quadro em Viridian eram iguais aos do quadro
  anterior (`LARECOMP_VCOPY_SAME`): malhas paradas, copiadas de novo todo quadro (~12 MB). Conferir com hash custava o
  mesmo que copiar.
- **Correção.** O heap do jogo é reservado à parte com `MEM_WRITE_WATCH` (o Windows marca as páginas escritas por
  qualquer um: o código do jogo, o nosso, o do sistema; `plat_written`). Uma cópia de vértices fica guardada em VRAM
  (duas áreas de 128 MB, staging + VRAM, como o anel) e é usada de novo enquanto nenhuma página dela foi escrita depois
  dela (uma época por página, anotada a cada consulta). O que o jogo reescreve a cada quadro (encontrado escrito em duas
  consultas seguidas) vai para o anel como antes, testado de novo a cada 64. Quando a área ativa enche, a outra é
  aposentada (suas cópias deixam de ser usadas) e esvaziada quando a GPU terminou de lê-la, sem espera. O serviço de
  arquivos reescreve um byte de cada página lida para a memória do jogo, para o rastreio vê-la com certeza. Heap que
  muda de tamanho ou reboot: tudo descartado. `LARECOMP_VK_KEEP_VERTICES=0` / `LARECOMP_WRITE_WATCH=0` desligam.
- **Erros no caminho** (pegos nas medições, corrigidos antes do commit): esperar a GPU por uma área usada pelo quadro
  ainda não enviado travava 10 s (Floresta); com as duas áreas lidas todo quadro, nenhuma ficava livre e o cache parava
  de aceitar cópias; o contador de intervalos voláteis deixava recopiá-los a cada consulta.
- **Medido.** Envio de vértices por quadro em Viridian 12,5 -> 2,6 MB (~4 GB a cada 2 s lidos das cópias guardadas).
  A/B intercalado, mesmo exe, janela em primeiro plano, 4 + 4 rodadas: Viridian Eevee parado 201 -> 224 fps (razão
  1,110, IC 95% 1,102-1,124), andando 185 -> 215 (1,162; 1,148-1,169), Pikachu andando 187 -> 215 (1,151;
  1,145-1,162); Floresta +7-10% nas rodadas comparáveis (a câmera desse save varia). O rastreio sozinho, sem o cache:
  sem custo medido (190,1 x 189,5 fps). Bateria dos dois jogos com e sem o cache, 3 repetições: `bateria_scan cmp` OK
  nas 6 rotas, 0 crash; `capture_loop` na Floresta: o mundo volta igual depois dos encontros.
- **Limites.** Só Windows (o Linux não tem o rastreio: fica desligado). Importação do heap no Vulkan com a região
  rastreada testada só na NVIDIA: na 0.6.0 o rastreio (e com ele o cache) só liga sozinho quando o adaptador de vídeo
  principal é NVIDIA (`EnumDisplayDevices`, antes de o Vulkan existir); nas outras placas `LARECOMP_WRITE_WATCH=1` liga,
  e o log diz se ficou ligado. Numa placa que recuse a importação, o log mostra "guest heap import failed".

## O Finish de cada quadro: a thread de renderização deixava o jogo mais lento (23/10/2026)

- **Sintoma.** Sem limite de fps e com a GPU folgada (efeitos desligados, ou 4K com só SMAA), o jogo andava mais
  devagar com a thread de renderização do que sem ela: Pallet Town (Pikachu) 122 fps ligada contra 179 desligada,
  3 + 3 rodadas intercaladas. O contrário do que o item 59 mediu em Pewter.
- **Diagnóstico.** Amostragem das threads (ponteiro de instrução e endereços de retorno na pilha): nenhuma thread
  saturada (render 31-39 %, as três do jogo 21-31 %), todas esperando umas pelas outras. Contadores temporários no
  `nvnSyncWait` / `nvnQueueFinish`: um `nvnQueueFinish` por quadro, ~4,4 ms cada (esvaziar a fila da thread de
  renderização e depois esperar a GPU). Chamado sempre do mesmo ponto do laço de quadros do jogo (Pikachu
  `main+0x29eec`): `fdiv d0, d0, 1e6` / `fmov d1, #4.0` / `fcmp` / `b.pl` - o jogo só chama o Finish quando o tempo de GPU
  que ele mediu no quadro anterior (os dois `ReportCounter`, em ns) ficou abaixo de 4 ms. Os contadores medem o nosso
  tempo de tradução, sempre abaixo de 4 ms: Finish em todo quadro, e jogo, thread de renderização e GPU em série.
- **Correção.** Com a thread de renderização, esse Finish segue o caminho que o próprio jogo usa quando a GPU leva
  4 ms ou mais (sem Finish; os fences dele continuam valendo). O ponto é reconhecido pelas instruções (endereço de
  retorno e as quatro antes da chamada; padrão único no `main`), não por endereço. Os outros Finish (o mesmo laço
  chama outro quando o modo de vídeo muda) não mudam. `LARECOMP_FRAME_FINISH=1` volta ao antigo.
- **De passagem.** `CMD_FENCE_SYNC` (fence gravado numa lista) lia `x[1]` (a condição, sempre 0) em vez de `x[0]` (o
  sync): esses fences nunca eram marcados. Corrigido; com a thread de renderização o sync recebe, quando a lista é
  enviada, o ticket do trabalho que a executa (`nvn_list_*`, `objects.c`), e o `nvnSyncWait` espera só esse trabalho.
  O Let's Go não usa fences em listas (as esperas sem fence são do sync do `nvnWindowAcquireTexture`): sem efeito aqui.
  `rt_dispatch` (chamadas indiretas do jogo): a busca lenta saiu da função, que salvava 5 registradores e montava a
  pilha em todo acerto do cache; `rt_fnhook` olha a tabela de ganchos antes da variável por thread. +1,5 % (4 + 4).
- **Medido** (RTX 5070 Ti, 4K tela cheia, sem efeitos: `aa = off`, SSAO / GI / blur / sombras suaves desligados;
  sem limite de fps, rodadas intercaladas, mesmo save copiado): Pallet Town parado 145 -> 307-313 fps (3 + 3 e 4 + 4), menu
  aberto 140 -> 300, rota até a Rota 1 147 -> 260, andando em Pallet Town tempo médio por quadro 8,5 -> 5,0 ms (P95 8,4 ->
  4,7); sem thread de renderização 204 -> 211. Com limite: 30, 60 e 120 fps e geração de quadros x2 com o mesmo ritmo e
  P95/P99 da build anterior. Imagens dentro da variação entre duas rodadas da mesma build (PSNR 29-36 dB contra
  27-33); captura completa de um Pidgey na Rota 1 sem defeito.
- **Agora.** Com a thread de renderização ~50-70 % ocupada, o limite são as três threads do jogo (código recompilado
  ~87 % do tempo ativo delas). A função mais pesada, `f_7000608b30` (Pikachu), ~13 %: busca linear por hash que
  recalcula o FNV-1a do nome de cada candidato, byte a byte - o algoritmo do próprio jogo. `rt_fnhook` (todo início de
  função, porque sempre há ganchos) ~4 % e `rt_dispatch` ~3 %: baixá-los exige mudar o `FN_ENTER` e recompilar o
  código do jogo. Compilar o código do jogo com -O2 não muda nada: o `zig cc` do jogador gera objetos idênticos com
  -O1, -O2 e -O3.
