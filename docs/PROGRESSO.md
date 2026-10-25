# Progresso

Estado atual do port. A história completa, com cada problema e como foi resolvido, está em
[HISTORICO.md](HISTORICO.md). As ferramentas de diagnóstico estão em [DEPURACAO.md](DEPURACAO.md).
O backlog priorizado para uma versão madura está em [ROADMAP.md](ROADMAP.md).

_Atualizado em 17/10/2026._

## O que funciona
| Área | Estado |
|---|---|
| Boot | completo: 4 módulos, SDK, construtores, threads, serviços do sistema |
| Jogos | *Let's Go, Eevee!* e, desde a 0.2.1, *Pikachu!* (program id no `main.npdm`), em qualquer versão. Paridade: toda feature vale para os dois (CLAUDE.md); os endereços fixos (60 fps, batalhas selvagens, remendos do gerador) têm tabela por build para a 1.0.0 dos dois e o Eevee 1.0.2 (`runtime/fnhook.c`, `tools/patches.py`, mapeados com `tools/addrmap.py`); as outras atualizações (Pikachu 1.0.2, 1.0.1) rodam sem essas opções. Pikachu 1.0.0 testado: setup, jogo novo até o nome, mundo, menus, 60 fps, batalha selvagem e captura (save copiado do Eevee), tradução PT-BR, GO Park. Falta: jogo novo do Pikachu além do nome, Brincar com o Pikachu, Pokémon exclusivos |
| Controle | gamepad do PC como modo portátil (seleção de controle aceita) |
| Traduções / mods | pasta `mods/<nome>/` no formato do Atmosphere (LayeredFS): arquivos do romfs trocados ou adicionados, escolhida no launcher ("Tradução"). Testada com a tradução PT-BR de fãs (Pokémon Switch Brasil, v3.1): textos, menus e imagens em português. Instalar: botão "Instalar tradução..." do launcher com o arquivo baixado (.zip/.rar/.7z), ou copiar o `romfs` do Eevee para `mods/<nome>/romfs` |
| Release | `LetsGoRecomp.exe` num zip (`make dist`): o jogador escolhe a pasta do jogo no launcher, que confere a v1.0.0, copia os arquivos e recompila o código do jogo no PC dele (gerador congelado + Zig); launcher em inglês ou português |
| Tela de toque (Brincar com o parceiro) | mouse: botão esquerdo segurado é o dedo; controle: analógico direito move um dedo (círculo na tela) e ZR segurado toca. Acariciar, abrir a bandeja de frutas e levar a fruta até a boca funcionam (testado só com controle: o Eevee comeu). Funciona em qualquer tamanho e proporção de janela |
| Teclado e mouse | WASD anda / mira, Espaço ou Enter = A, Q ou Backspace = B, Tab = X (menu), R = Y, Z / C = L / R, `=` / `-` = Plus / Minus, setas = d-pad. Mouse: na mira da Poké Ball (depois do "Get ready") ele é preso à janela: movimento = mira (giroscópio virtual), esquerdo = arremessar; fora dela fica livre e o botão esquerdo é um dedo na tela de toque (Brincar com Eevee: acariciar); direito = B sempre. As teclas e a sensibilidade do mouse se trocam no launcher (página "Controles") |
| Vídeo de abertura | imagem e áudio corretos, 1080p, termina e segue para o jogo |
| Áudio | música e efeitos (Opus via `hwopus`), áudio dos vídeos sem cortes |
| Ritmo | 30 fps travados (o do jogo) por padrão; opção experimental de 60 fps no mundo (item 5) |
| Introdução | idioma, Professor Carvalho, escolha de personagem, nome digitado corretamente |
| 3D | cenário correto (quarto, Pallet Town, céu), passes de profundidade/sombra |
| Personagens e Pokémon | modelos com esqueleto corretos, com animação (correção do `LDC.64` no tradutor de shaders) |
| Gameplay | andar, encontro com o Professor, captura do Pikachu (mira centralizada) |
| Save | salvar e carregar funcionam (`save/save_0/savedata.bin`) |
| Batalhas selvagens (experimental, opção no launcher, desligada por padrão; `wild_battle = 1`) | encostar num Pokémon selvagem abre uma batalha clássica contra ele (espécie, nível, gênero, IVs e golpes por nível reais) e, depois, a captura; o mundo volta com fade in. Desde 23/10/2026, o 5º botão CATCH (nativo, 9 idiomas): captura a qualquer momento, chance maior com o selvagem fraco ou com status, uma bola por vez (escapou → a batalha volta com a mesma vida), desmaiou = perdido, mas o time ganha a XP nativa (e evolui depois da batalha); sem o relógio de 5 minutos; também com o inicial andando sozinho; o selvagem derrotado desmaia; a música da batalha segue pela captura e pela volta (24/10/2026); "Back to battle" na captura e volta sem a
abertura nem arremesso; testado por roteiro no Eevee e no Pikachu. Falta a abertura da captura, natureza/shiny, a espera de ~12 s ao sair da batalha e mais áreas (HISTORICO, [MODO-BATALHA.md](MODO-BATALHA.md), etapa 6) |

## O que falta
1. **Validar o 3D em mais cenas** (batalhas, Pokémon grandes, cidades): a correção dos modelos com esqueleto foi
   validada na Pallet Town. Histórico em [HISTORICO.md, parte 6](HISTORICO.md#parte-6-personagens-explodidos-resolvido).
2. **Mira por movimento**: mouse (giroscópio virtual) e giroscópio real do controle (SDL, escrito em 16/10/2026 e guardado fora do `main`
   no `git stash` "giroscopio do controle"; **não testado com um controle de verdade**: sentido dos eixos a confirmar; `LARECOMP_GYRO_PAD_FLIP=1|2|3` inverte
   yaw/pitch). Sensibilidades no launcher. Falta: correção de deriva do pitch pelo acelerômetro.
3. **Recursos online** (Pokémon GO, trocas, Poké Ball Plus): desligados de propósito (serviços offline).
4. **Desempenho** (medido em 15/10/2026, `tools/perf_summary.py` sobre 4 min andando pela Rota 1): 30 fps estáveis
   (mínimo 28,6 por janela de 2 s, nenhum quadro acima de 50 ms fora o print do teste, nenhum underrun de áudio) numa
   RTX 3080. Ao entrar numa área, ~0,3 s de objetos aparecendo enquanto os shaders novos ficam prontos (com cache: ~200 ms
   de tradução e ~90 ms de SPIR-V para ~380 programas); na primeira vez sem cache, alguns segundos. O cache de pipelines
   agora é salvo durante o jogo, não só ao fechar. Na gameplay, um draw espera seu shader (até 20 ms por quadro) em vez de
   sumir: zero draws pulados depois do carregamento. A tradução de shaders roda em paralelo: mesmo na primeira visita
   (cache vazio) a área aparece completa depois do carregamento. Qualidade: SMAA ligado, filtro anisotrópico 16x e supersampling bem
   reduzido (1440p / 4K no launcher numa tela menor; 4K roda a ~78 fps sem limite numa RTX 3080). Detalhes em
   [HISTORICO.md, parte 8](HISTORICO.md).
   Auditoria de 17/10: a resolução dinâmica tratava 30 fps estáveis como lentidão de 60 Hz; o orçamento foi corrigido
   para 30/60 conforme a cena. Teste isolado e Eevee roteirizado em 30/60 (refeito com ini LF: antes descia a 0,60, agora fica em 0,95-1,00) passaram; a opção só existe no ini (`resolution = dynamic`); coleta opcional de frametimes dá
   média e P95/P99 ([performance-analysis.md](performance-analysis.md)). Ainda faltam Pikachu, transições e profiling de CPU/GPU.
   Perfil de 17-18/10 (Eevee, RTX 3080, mundo da Rota 1): sem limite de fps o mundo rodava a ~60 fps (a nota de ~160 fps
   acima não se repetiu nesta rota); duas causas achadas e corrigidas: espera por mutex no kernel emulado (HISTORICO 57: o
   60 fps pacado passou de 58,8-59,3 para 60,0 fps) e leitura de vértices pela GPU via PCIe (HISTORICO 58: GPU de ~10 para
   ~1,2 ms por quadro, fps sem limite ~75 -> ~90). O limite agora é a cadeia de CPU entre as 3 threads do jogo
   ([PESQUISA-GPU.md](PESQUISA-GPU.md)). Sem teste em GPU mais fraca nem em placa integrada.
5. **60 fps (experimental, opção "60 fps (experimental)" no launcher, desligada por padrão; `fps = 60` no larecomp.ini ou `LARECOMP_FPS60=1`)**: mundo a 60 nativo, menus e Pokédex a 30 com quadro intermediário, batalhas e capturas a 30 (HISTORICO 43-49). Histórico da investigação: a lógica do jogo anda um passo por quadro. Com `LARECOMP_FPS=60` ele chega ao mesmo estado
   no mesmo número de quadro em metade do tempo, ou seja, roda em velocidade dobrada (medido em 15/10/2026 com
   `tools/repro.sh`). 60 fps exigiria mudar o passo de tempo do próprio jogo. O que já se sabe:
   - O motor escolhe o intervalo de present no fim de quadro (`0x7000029e80`, função `0x7000029d30`):
     `clamp(config[+8] / (G − 1), 1, 5)`, com `G` = 16.666.667 ns (um vsync de 60 Hz, ponteiro em `0x70015fb438`). O
     resultado é 2 e só é aplicado uma vez (`SetPresentInterval 2`, via `0x70005e79c0`); mesmo pacejado a 60, o jogo não
     pede intervalo 1, então `config[+8]` é fixo (~33,3 ms), não uma medida.
   - O único valor perto de 33,3 ms achado nos dados do `main` e no heap (`33.412.440` em `0x727eeccb68`) não é esse:
     trocá-lo por 16.666.667 não muda nada.
   - O float 1/30 aparece uma vez (`0x7000f47428`), usado só numa função de interpolação (`0x7000611440`), não no laço.
   - O laço do aplicativo (`0x70000231a0`) chama a atualização (`0x700001d1c0`) e depois o fim de quadro por vtable.
   - ~~Não há passo de tempo global~~ (**corrigido em 16/10/2026**: a varredura procurava floats e não achou porque o
     passo é um inteiro em nanossegundos, numa pilha). Existe uma referência temporal: a tabela de durações `G`
     (`main+0xf7eeb8`: 16,7 / 33,3 / 50 / 66,7 / 83,3 ms, ponteiro em `0x70015fb438`) e um objeto de temporizadores na
     pilha da thread principal com dois passos. O passo 1 é reescrito a cada atualização do mundo pelo accessor
     `f_7000039600` com `G[1]`; o passo 0 fica em 33,3 ms. Os sistemas que usam o **valor** do passo (animações
     `f_7000663660`/`f_7000664cd0`, `f_700067be50`, `f_70005ed1e8`/`f_70005ed908` e outros) leem o passo 1 e
     convertem tempo em quadros dividindo por `G[1]`; é por isso que trocar `G[1]` no accessor deixa o mundo certo a
     60. O que não passa por aí: contadores inteiros de quadro (as linhas do tempo da captura e da batalha) e o
     passo 0, que só decide se um objeto atualiza ou não. Detalhes e medições: [PESQUISA-60FPS.md](PESQUISA-60FPS.md),
     "Mapa temporal".
   - Conclusão: 60 fps na velocidade certa não sai de um patch. Os caminhos seriam gerar quadros intermediários
     (interpolação no runtime, sem vetores de movimento do jogo: artefatos, principalmente na interface) ou um
     programa externo de geração de quadros. Pesquisa das técnicas usadas por outros ports: [PESQUISA-60FPS.md](PESQUISA-60FPS.md).
   - **Protótipo de interpolação removido** (15/10/2026): reexecutar cada quadro com os uniforms misturados funcionou
     para a câmera, mas sem marcações do próprio jogo (como o RT64 tem) deixava erros visíveis - objetos repetidos
     trocados (fantasmas), menus em branco, modelos esticados, clarões em efeitos. O jogo fica nos 30 fps originais.
6. **Aura dos Pokémon especiais pisca** (também na release, a 30 fps): no vídeo do jogador (17/10/2026) a aura some por
   exatamente 1 quadro do jogo, em intervalos irregulares, com o Pokémon no lugar. Descartados com medição: carimbos de
   tempo da GPU (`LARECOMP_VK_COUNTER`), espera de sync, cópia de textura para a memória, interpolação, e um relógio do
   sistema cravado por quadro (testado em jogo pelo jogador: igual). Ferramentas para continuar: `LARECOMP_COUNT_TEX=1024x256`
   e `LARECOMP_COUNT_FS=<hash>` (desenhos por quadro), `LARECOMP_TRACE_COUNT` (listas que contêm um desenho e quem as
   chama), `LARECOMP_CALLCOUNT`, `tools/blink_scan.py`. A rota de teste não tem Pokémon com aura; a gravação F9 altera o
   próprio resultado (o primeiro quadro gravado perde os efeitos), então serve para estrutura, não para achar a piscada.

## Correções no runtime (resumo)
| Área | Correção | Arquivo |
|---|---|---|
| Kernel | contexto de exceção (`svcGetInfo` 20), Title ID do `main.npdm` | `runtime/kernel.c`, `main.c` |
| Kernel | condvar não readquire o mutex no timeout; não sobrescreve um "acordado" com timeout | `runtime/kernel.c` |
| Loader | 4 módulos (`main`, `sdk`, `subsdk0` glslc, `subsdk1` multimedia) | `runtime/loader.c`, `dispatch.c`, `gamelib.*` |
| Serviços | Bluetooth offline (`btm:u`, `bt`), `csrng`, `hwopus` | `runtime/services/offline.c`, `hwopus.c` |
| Vídeo | buffers do jogo no `GetOutputBuffer`, slots em rodízio, `FindInt64`/`SetCoreMask`, fim de stream `0x2000`, alocador do jogo | `runtime/hle/movie.c` |
| `vi` | `SetLayerScalingMode` | `runtime/hle/vi.c` |
| Controle | modo portátil, estilos suportados pelo jogo, teclado / mouse, giroscópio virtual movido pelo mouse | `runtime/hle/input.c`, `runtime/hle/bindings.c`, `services/hid_set.c` |
| Teclado | nome em UTF-16 | `runtime/hle/swkbd.c` |
| NVN | programas por estágio, programas só com vertex shader, pacing pelo present interval | `runtime/nvn/cmdbuf.c`, `objects.c`, `vulkan/exec.c` |
| Tradutor de shaders | `LDC.64` cujo registrador de endereço também é destino (modelos com esqueleto) | `runtime/nvn/maxwell/translate.c` |
| Texturas | `RGB5A1`/`RGBA4` convertidos para RGBA8 (ícones dos Pokémon) | `runtime/nvn/vulkan/textures.c` |
| Texturas | sem escala para texturas preenchidas pela CPU; invalidação por escrita do host | `runtime/nvn/vulkan/textures.c` |
| Áudio | fila mínima no `audout` + contagem de underruns | `runtime/services/audio.c` |
