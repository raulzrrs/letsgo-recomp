# Modo de jogo: batalhar com Pokémon selvagens

_Ideia (15/10/2026): ao encostar num Pokémon selvagem, em vez de ir direto para a captura (estilo Pokémon GO), entrar
numa batalha clássica - e capturar depois, como o jogo já faz com os lendários e o Snorlax. Até onde se sabe ninguém
fez isso em mod do Let's Go ([discussão de 2026](https://reddit.sentinel-team.org/posts/1rr2xif/snapshots/2026-03-13T05%3A19%3A13.96841Z))._

## Etapa 1: mapa dos dois caminhos ✅

Método: `LARECOMP_FNPROF` (runtime/fnprof.c: funções do jogo executadas numa janela de frames, agora com a ordem da
primeira entrada e o chamador) em rodadas sem encontro, com encontro selvagem e com batalha de treinador (save na
Rota 1, ao lado do Youngster Ronny); `tools/fncompare.py` separa o que é comum e o que é de cada evento, e
`LARECOMP_FNPROF_BT=<addr>` + `LARECOMP_FNPROF_BT_MAX=<n>` mostram a cadeia de chamadas.

Atualização do mapa (todo quadro): `main+13c8f0` → `13d2f0` → `315390` → `2f78d0` → **`2f8bb0`**, que chama:
- **`2fda00`** (Pokémon selvagens no mapa). No encostar: → **`305ad0`** → **`28ef70`**`(x0 = objeto do encontro,
  x1 = gerenciador global [*0x70015faab8]+0x68)`, que aloca a cena de captura (0x3B0 bytes) e a empurra:
  `31c030` → `139e80` (empilhar processo/cena).
- **`2facf0`** / `2fa860` (visão dos treinadores). Ao ver o jogador: `17a7f0` → `17aa70` → `17b380` → `139e80`
  (evento: "!" e diálogo).

Batalha de treinador, depois do diálogo: interpretador de script `17b6c0` → `19ca10` → `19b7f0` → comando
`674ed0` → `672484` → `66fc20` → `17e150` → **`28bb60`**`(x0 = &objeto, x1, x2 = &parâmetros)`, que copia uma
configuração de batalha de **0x30 bytes** para o objeto da batalha (+0x88) e troca de cena (`28bc30` → `28e480` →
`139e80`); dentro da batalha, `28be80` → `28ccb0` → `28b150` → `31a470`.

Conclusão: captura (`28ef70`) e batalha (`28bb60`/`28e480`) são métodos vizinhos do mesmo gerenciador de encontros
(`main+28xxxx`). O desvio seria em `305ad0`: montar uma configuração de batalha selvagem para o Pokémon tocado e
chamar o início de batalha no lugar da captura; depois da batalha, iniciar a captura (o fluxo dos lendários).

## Etapa 2: ganchos e primeiros dados ✅

- **Ganchos de função** (runtime/fnhook.c, `FN_ENTER` em runtime/cpu.h, ABI da DLL 3): o runtime pode tratar ou
  envolver qualquer função do jogo sem gerar a DLL de novo. `LARECOMP_HOOK_DUMP` registra argumentos e memória.
- **Batalha do Youngster Ronny** (`28bb60`, chamado só pelo comando de script `17e150`):
  - `x2` = parâmetros de 0x30 bytes: `{0, 0, 0x1C1, 0, 0x200, ...}`; o comando de script recebe os mesmos 0x1C1 e
    0x200 - **0x1C1 (449) deve ser o treinador**, 0x200 flags;
  - `*x0` = objeto grande com contagem de referências (vtable `0x7001537770`): o pedido de batalha do evento.
- **Outros caminhos** (referências diretas no `main`, `tools/xrefs.py`):
  - `28bc30` (inicia a batalha de fato) tem dois chamadores: `28bb60` (script) e **`28bda0`**, chamado por
    `289c4c` - um método virtual que monta os parâmetros com um **modo 0 ou 3** conforme a flag `[obj+0xA6]`;
  - a captura `28ef70` tem quatro chamadores: o encontro selvagem (`305ac4`), `28ea90`, `854a48` e **`31a0e0`**,
    chamado pelo método virtual `319e20`, ao lado da sequência de batalha (`31a470`) - candidato a "batalha e depois
    captura" dos lendários.

## Etapa 3 (parcial, 17/10/2026): o que a pesquisa estática e os dumps mostraram
**Chamadores conferidos** (busca de `bl` direto para o endereço exato; o `xrefs.py calls` erra o início da função
quando há padding antes dela):
- `28ef70` (captura): `28ead8`, `305b34` (encontro selvagem), `31a140` (fn `31a0d8`), `854afc`. Confere com a etapa 2.
- `28bc30` (iniciar batalha): só `28bb80` (script) e `28bdbc`; `28bb60` só `17e218`; `28bda0` só `289ce0`.

**`289c4c` e `319e20` não são métodos soltos**: são casos da jump table de máquinas de estado (estado em
`[obj+0x68]`) de duas classes com vtable própria. Classe A: vtable em `image+0x1546ea8` (slot da GOT `0x15ff050`),
construtores a partir de `289ac0`, fábrica `17f098` (aloca `0xA8` bytes). Classe B: vtable em `image+0x154cb70` (GOT
`0x15ff8c0`), construtor `318f50`, fábrica `291470` (aloca `0x100` bytes). As fábricas seguem o mesmo molde (alocar,
construir, empilhar com `13a750(gerenciador, &objeto)`), o que sugere uma família grande de "processos".
- Classe A, caso `289c4c`: monta `{u32 modo, ponteiro config}` com modo `3` se `[obj+0xA6] != 0`, senão `0`, config =
  `[obj+0x90]`, e chama `28bda0`, que inicia a batalha (`28bc30`) e grava modo em `battle+0xb8` e config em `+0xc0`.
  Depois chama `28dd20`/`28dd40` com as flags `[obj+0xa0..0xa2]`.
- Classe B, caso `319e20`: chama `31a0e0` (que chama a captura `28ef70`) e devolve estado 4.
- Ainda **não confirmado em execução** que A + B formem o fluxo "batalha e depois captura" dos lendários, e não sei
  quem instancia as fábricas (são métodos virtuais, sem `bl` direto).

**Dump de um encontro selvagem real** (`LARECOMP_HOOK_DUMP`, frame 1545 da Rota 1): `305ad0` recebe `x0` = objeto do
Pokémon (vtable `0x70159c848`, campos `+8 = 2`, `+0x28 = 4`), `x1` = gerenciador; `28ef70` recebe `x0` = objeto de
vtable `0x7001537770` (**a mesma classe do "pedido de batalha" do `28bb60`**) e `x1` = alocador do gerenciador global.
Quem identifica a espécie/nível do Pokémon tocado ainda não foi lido.

**Armadilhas dos testes** (para não repetir):
- `LARECOMP_HOOK_DUMP` exige o endereço **completo do guest** (`700028ef70`), não o offset do `main` (`28ef70` não
  dispara nada, sem erro).
- O `fps60.c` registra ganchos em `28ef70` e `28bb60` quando `LARECOMP_SHOT_REL` está ligado, e o `teste60.sh` sempre
  liga: o último gancho registrado vale, então o dump é anulado. Use `tools/repro.sh` direto com o roteiro.
- O sorteio do encontro muda o resultado do mesmo roteiro: o "captura" às vezes cai numa batalha de treinador e o
  "batalha" às vezes numa captura. Confira a imagem final ou o log antes de concluir.
- Worktree: `extracted/`, `generated/` e `.venv/` entram como junctions; copie `build/lasgame.dll` **e
  `build/lasgame.dll.id`** (sem o `.id` o runtime recusa a DLL: "built for another version") e compile só o runtime
  (`make -j20 build/lasrecomp.exe`).

## Etapa 3b (17/10/2026): o que a comunidade já tem, e a tabela das batalhas "estáticas"
**Comunidade** (pesquisa em 17/10/2026): o [pkNX](https://github.com/kwsch/pkNX) edita o Let's Go por **dados do
romfs** (caminhos em `pkNX.Game/File/GameFileMapping.cs`, `FilesGG`): encontros estáticos
(`bin/script_event_data/event_encount.bin`), presentes (`add_poke.bin`), trocas (`field_trade_data.bin`), tabelas
de selvagens (`bin/field/param/encount/encount_data_{p,e}.bin`, FlatBuffers `wild.fbs`), treinadores
(`bin/trainer/trainer_{data,poke,type}`). Taxa de shiny, TMs e tabela de tipos ficam no `main` e o pkNX os acha por
padrão de bytes e os altera (`EditorGG.cs`). O mod de dados [Peerless Pikachu & Exquisite Eevee PLUS](https://www.pokecommunity.com/threads/pok%C3%A9mon-lets-go-peerless-pikachu-exquisite-eevee-plus.541476/)
(feito com pkNX) mexe em selvagens, estáticos e treinadores, mas na [discussão do lançamento](https://reddit.sentinel-team.org/posts/1rr2xif/snapshots/2026-03-13T05%3A19%3A13.96841Z)
um comentário diz que ninguém achou como pôr batalhas selvagens comuns, e vários pedem isso. Ou seja: **editar a tabela
dos estáticos é conhecido; transformar um selvagem comum em batalha ninguém fez** (e nada nas fontes diz que seja
impossível).

**A tabela das batalhas estáticas, no nosso romfs** (`extracted/romfs/bin/script_event_data/event_encount.bin`, 10
registros de 0x40 bytes; layout `EncounterStatic7b` do pkNX: `+0` hash u64, `+8` espécie u16, `+0xA` forma, `+0xB`
nível, `+0xC` shiny, `+0xD` gênero, `+0xE` natureza (25 = aleatória), `+0xF` habilidade, `+0x20` 4 golpes u16, `+0x2C`
IVs, `+0x32` EVs, `+0x38` aumentos de status, `+0x10..0x1F` ponteiros de texto):
| # | Pokémon | Nível |
|---|---|---|
| 0 / 1 | Pikachu (forma 8) / Eevee (forma 1) iniciais | 5 |
| 2 / 3 / 4 | Articuno / Zapdos / Moltres | 50 (+1 em todos os status) |
| 5 | Mewtwo | 70 (+1 em todos) |
| 6 / 7 | Snorlax (dois, +1 Atk / +1 Def) | 34 |
| 8 / 9 | Voltorb / Electrode | 42 |
Cada batalha é achada por um **hash u64** que vem do script do evento. Isto é o "pedido de batalha selvagem" que
procurávamos: a espécie, o nível, o gênero, a natureza, os IVs e os golpes de um selvagem cabem num registro assim.

Pistas no `main` (tabela de strings perto de `image+0xf04100`): nomes de cenários `sinario_densetu_mae` e
`sinario_densetu_hokaku` (densetsu = lenda, hokaku = captura) ao lado dos três caminhos de arquivo acima. O código não
referencia os nomes por `adrp`+`add` nem por ponteiro direto (a busca não achou nada), então o carregador usa índices.

**Plano revisado do protótipo**: em vez de montar o pedido de batalha do zero, reaproveitar o comando de script das
batalhas estáticas com um **registro sintético** (copiar o do Snorlax e preencher espécie, forma, nível, gênero,
natureza, IVs e shiny do Pokémon tocado; os golpes vêm da tabela de aprendizado pelo nível). Falta achar: o carregador
da tabela (a tabela fica em memória depois de lida: dá para achar o endereço varrendo o heap pelo hash do Snorlax
`343e35139d576e27`) e o comando de script que recebe o hash. Outro caminho já provado pela comunidade: cheats e mods
de "teletransporte" e de "lendário aparecer" mexem em posição e em flags (`bin/flagwork`); como a memória do guest é
memória do host, o runtime pode fazer o mesmo para **chegar a uma batalha nativa de Snorlax/lendário** e observar o
fluxo de verdade, sem save adiantado.

## Etapa 3c (17/10/2026): quem lê a tabela por hash = a classe A, o processo das batalhas estáticas
(Correção: os "leitores" do `event_encount.bin` aparecem por `adrp`+`add` no início da **string** `+0x156` da tabela de
strings em `image+0xf04000`, e não no ponto onde o nome do arquivo começa dentro dela, que foi o meu erro na busca.)

A tabela **não está na memória durante o campo**: só `field_trade_data.bin` é lido no boot (`LARECOMP_LOG_FILES=1`);
`event_encount.bin` e `add_poke.bin` são lidos **sob demanda**, quando um evento os pede. Varredura de memória do guest
pelo hash do Snorlax no frame 1400 da Rota 1: 0 acertos (`LARECOMP_SCAN`, ver abaixo).

O código, no `main`:
- **Carregador da tabela** (classe com objeto de `0x140` bytes, ctor `2ba800`): `2ba920(this)` pede o arquivo
  `bin/script_event_data/event_encount.bin` (string `+0x156`, via `70001d5150` e `70000a63d0`); `2baae0` diz se já
  carregou (`1` quando `[this+0x130]` ou o arquivo pronto); `2bab70` percorre `tamanho >> 6` registros de 0x40 bytes,
  converte cada um numa estrutura de 0x59 bytes (hash, espécie, forma, nível, shiny, gênero, natureza, habilidade,
  golpes, IVs, EVs, aumentos) e insere num `unordered_map` com chave = hash (`2baea0`). **`2bad00(tabela, &chave)`** é a
  busca: devolve `nó + 0x18` (o ponteiro para a estrutura). Usado só pelas classes dos estados `289b40` (A), `28e99c`
  (`28ea90`, que chama a captura `28ef70`) e `28f454`.
- **Classe A** (vtable `image+0x1546ea8`, objeto de `0xA8` bytes, fábrica `17f098`) é o processo da batalha estática:
  `[+0x68]` estado; `[+0x88]` objeto da tabela; `[+0x90]` configuração de batalha (`0x2b8` bytes); **`[+0x98]` o hash
  (u64) da batalha**; `[+0xa0..0xa2]` e `[+0xa6]` flags. Estados: 0 (`289b7c`) cria o objeto da tabela e pede o arquivo;
  1 (`289e18`) espera (`2baae0`); 2 (`289e40`) aloca a configuração, **busca por `[+0x98]`** (`2bad00`) e preenche a
  configuração com `2f60c0(config, registro)` (ela grava `[config+0x298] = 1` e copia espécie, nível etc.); 3 (`289c4c`)
  inicia a batalha (`28bda0`, modo 3 se `[+0xa6] != 0`, senão 0) e depois `28dd20`/`28dd40`; 4 não lido.
- A fábrica `17f098` é chamada de um método virtual (o comando de script do evento), que recebe o gerenciador de
  processos e o alocador e depois preenche o hash e as flags no objeto devolvido (a confirmar).

**Consequência para o protótipo** (duas opções, a segunda é a mais limpa):
1. Criar a classe A pela fábrica `17f098` e gravar o hash do Snorlax em `[obj+0x98]` (batalha nativa de teste).
2. **Gancho em `2bad00`**: se a chave for um hash nosso, devolver um registro sintético (estrutura de 0x59 bytes,
   mesma disposição do parser) com a espécie, nível, gênero, natureza e IVs do Pokémon tocado; o resto do fluxo
   (configuração, batalha, captura depois) é o do jogo.

**Ferramenta nova**: `LARECOMP_SCAN="<bytes hex>[@<frame>]"` (+ `LARECOMP_SCAN_BACK=<n registros>`) varre a memória
mapeada do guest (Windows, `runtime/hle/memscan.c`) e loga cada acerto, os 0x40 bytes e quem aponta para a base.

> **Estado em 17/10/2026:** funciona (experimental). Resumo, sintomas e correções no [HISTORICO.md](HISTORICO.md), seção
> "Modo batalhas selvagens". As etapas abaixo são o diário da pesquisa.

## Etapa 4 (17/10/2026): o protótipo funciona; os nativos de script do jogo
**Protótipo** (`runtime/hle/wildbattle.c`, `LARECOMP_WILDBATTLE=1`): o gancho no `305ad0` (o gatilho do toque num
selvagem, que chama a captura e depois configura o handle devolvido, por isso é substituído inteiro) cria o processo da
classe A pela fábrica `17f0a0`, o gancho no construtor `289ac0` grava o hash nosso em `[+0x98]` e o gancho em `2bad00`
responde a esse hash com uma cópia do registro real do Snorlax (molde) com espécie, nível e golpes trocados. Jogado por
um humano na Rota 1: encostar num Pokémon abre a batalha (Rattata nível 3 de teste) e, ao fim, a captura da espécie da
batalha. Problema aberto: tela preta ao voltar ao mundo (o mundo é desenhado, os passes finais saem pretos: um fade).

**Os nomes dos nativos de script estão no `main`** (strings em `image+0xeff9xx..0xf01dxx`, ponteiro para a função no slot
seguinte da tabela de nativos): `CallTrainerBattleCore` `17e150`, **`CallWildBattle` `17e440`**, `GetTrainerBattleResult_`
`17e700`, `GetWildBattleResult_` `17e7c0`, `CallBattleLose_` `17e880`, `SaveEventEncountPokemonParam` `17ed00`,
`ClearEventEncountPokemonParam` `17ee70`, `DisappearEncountSymbols` `185cc0`, `EncountPokeSetVisible` `189fe0`,
`FadeIn_` `1a7680`, `FadeOut_` `1a7860`, `FadeWait_` `1a7aa0`, `FadeCheck_` `1a7ad0`. Os scripts são AMX (Pawn) em
`bin/archive/script_amx.gfpak` (GFLXPACK: 705 arquivos, LZ4; formato no `pkNX.Containers/Misc/GFPack.cs`) e **não guardam
nomes**, só hashes.
- **`CallWildBattle(amx, params)`**: params = `{[1] hash u64, [2] flags u32}`. Cria a classe A (fábrica `17f0a0`) e grava
  `[+0x98] = hash`, `[+0xa0] = [+0xa1] = 0`, `[+0xa2] = 1`, `[+0xa3] = !(flags>>3 & 1)`, `[+0xa4] = !(flags>>1 & 1)`,
  `[+0xa5] = !(flags>>2 & 1)`, `[+0xa6] = 1` só num caso especial (senão 0), e suspende o script (`672e50(amx, 0xc)`) até o
  processo acabar. Os padrões do **construtor** (`a0 = a1 = 1`, `a2 = 0`) são o oposto: o primeiro protótipo os deixava assim.
- `FadeIn_(amx, params)`: `{[1] quadros, [2] hash FNV-1a 64 do nome do tipo}` -> índice 1..11 -> função do motor
  `11ece0(gerenciador, 0, tipo, quadros)`; `FadeOut_` termina em `11ee50(gerenciador, 0, tipo, ...)`; gerenciador de fade
  = `[[*GOT 0x70015facf8]]`. Nomes dos tipos (strings em `image+0xefd685..0xefdb2e`): `fade_front_{fade,wipe,r,trainer,pika,
  bui,poke_get,legend,legend_get,loading}_{in,out,keep}`.
- O script faz FadeOut -> CallWildBattle -> (espera) -> GetWildBattleResult_ -> FadeIn..., o que o protótipo não faz.

## Etapa 5 (17/10/2026): XP dividido, o que se sabe e o que não deu
Objetivo: metade do XP ao "derrotar" (no fluxo dos lendários o Pokémon não desmaia: a batalha termina e começa a captura) e
a outra metade na captura. **Não implementado.** O que a pesquisa trouxe:
- A tela de recompensa da captura mostra o XP: **Metapod nível 5 = 20**, **Pidgey nível 4 = 11** ("Bonus Multiplier 1.0,
  Successful Catch", "Exp. Points"); depois vem a tela da equipe ("Oddish grew to Lv. 9!"). Uma batalha de treinador mostra
  "Your party Pokémon in waiting also got Exp. Points!" (o XP de batalha é concedido por outro caminho).
- Não achei a função que soma o XP. Tentativas: `PokePartyAddParam` (`197bb0`, nativo de script) delega a uma função por membro
  (`19aab0`) e ao objeto Pokémon (`[obj+0x98]` = CoreParam; `727fe0` é amizade, `725100`/`725800` EVs/IVs por índice);
  as constantes de XP do nível 100 não aparecem no código (as curvas vêm de `bin/pokelib/grow_table`); a comparação de perfis
  de funções (fugir x capturar) e a busca de PB7 no heap deram ambiguidade/falsos positivos; as fotos do heap
  (`LARECOMP_SNAP`) funcionam mas o heap tem contadores demais para isolar o delta.
- Caminhos que ainda valeriam: vigiar (`LARECOMP_WATCH`) o XP de um membro **conhecido** com o endereço achado por uma
  captura jogada à mão no mesmo boot; ou achar a função que escreve a tela "Exp. Points" (o valor 20/11 passa por um
  `SetNumber` de mensagem); ou somar o XP por conta própria depois de achar o campo.

## Etapa 6 (22/10/2026): capturar no meio da batalha, com a vida e o status contando
Objetivo (pedido do usuário): uma opção nova e nativa no menu da batalha ("Capturar"), uma bola por vez, chance maior com
o selvagem fraco ou com status, desmaiou = perdeu a chance. Tudo abaixo foi visto em execução (save `build/save-edit`,
Rota 1, Bulbasaur andando junto), salvo onde diz "estático".

**O motor de batalha não tem mais o arremesso clássico.** As sequências de bola em `bin/battle/waza/sequence` (`ee410`,
`ee412`, `ee420`, `ee422`, `ee430`..`ee460`) são todas de mandar o Pokémon à luta (`to_ee400_ball`,
`Play_SE_battle_sys_018_ball_throw`); não há sacudida nem captura. A captura é sempre a cena estilo GO.

**Do fim da batalha à captura** (estados do evento de batalha `28be80`, palavra `[+0x68]`; `[+0x720]` a batalha):
- 0 prepara, 1 espera o fade, 2/3 criam a batalha, **4 batalha correndo**, 5 fim: cria o processo pós-batalha
  (classe B, update `319bc0`) com `[B+0x88]` = a batalha e `[B+0xb8]` = tipo (`[evento+0xd0]`, 2 aqui);
  6/7/8/12 encerram e o processo da classe A termina.
- O **resultado** fica em `[[evento+0x720]+0x68]+0x508`: **1 vitória, 3 fuga**. O estado 0 da classe B, com tipo 2,
  vai para o estado 3 (`319e20` → `31a0e0` → `28ef70`, **a captura**) se o resultado for 1, senão para o 4 (fim).
- Prova: com a fuga trocada por vitória no estado 5 (o que o modo faz hoje quando o jogador pede a captura), o Rattata com a vida cheia
  saiu da batalha pelo RUN e o jogo abriu a captura nativa ("Oh! You encountered a wild Rattata!" → capturado).

**A vida e o status do selvagem.** O objeto da biblioteca que vai à captura (`[B+0xc0]`) **não** recebe a vida da
batalha (21/21 depois de o selvagem desmaiar). Quem tem a vida é o Pokémon de batalha (BTL_POKEPARAM): no fim da
batalha, vitória **ou fuga**, `9e4280(bpp)` (ReflectToPP) grava de volta na biblioteca, um por Pokémon (os do jogador e
por último o selvagem). Campos: `+0x60` o objeto da biblioteca, `+0x78` espécie (u16), `+0x7c` HP máximo, **`+0x7e` HP
atual**, condições nos 3 bits baixos de `+0xa0` (paralisia), `+0xa8` (sono), `+0xb0` (congelado), `+0xb8` (queimadura),
`+0xc0` (veneno), na ordem 1..5 da biblioteca. Medido: fuga com o Rattata em 10/21; vitória com ele em 0/20.
- Biblioteca (pml): `CoreParam` = objeto com o acessor em `+0x98`; acessor `[+0x10]` bloco principal (chave u32 em +0,
  0xE0 bytes cifrados a partir de +8), `[+8]` bloco de batalha (0x1C bytes: status u32, nível +4, HP +8, HP máximo +0xA,
  como o PB7 em +0xE8), cifrados com a mesma chave enquanto `[acc+0x18]` (LCG `0x41C64E6D`/`0x6073`, `739640`).
  Espécie `724950`, GetHp do acessor `729ea0`, GetMaxHp `729fc0`, SetHp `730de0`, `CoreParam::SetHp` `7265d0`.

**A fórmula de captura** (`7de080`, chamada a cada quadro da cena, também para a cor do anel): lê espécie/forma
(`+0`, `+4`), bola (`+0xc`) e fruta (`+0x10`) por tabela, os multiplicadores do arremesso (`+0x14..+0x20`), o nível
(`+0x24`, até 100) e **a taxa da espécie** por `71fc30(8)` → `71dfd0` (GetPersonalParam: `[this+0x10]` = registro de
0x54 bytes do `personal_total.bin`, **id 8 = taxa de captura**, o byte +8). Pidgey/Rattata 255, Pikachu 190, Eevee 45,
Snorlax 25. Ninguém mais pede a taxa durante a captura (registro com `LARECOMP_WILDBATTLE_CATCHLOG=1`). Obs.: a cópia
crua do `personal_total.bin` no heap (`0x729e0ea268`) não é lida por ninguém (vigia de leitura).

**O menu de comandos** (quadro "battle act select main frame", fábrica `aaa860` → `aaac10`, objeto de 0x190 bytes,
construtor `aab290`, vtable pela GOT `0x7001608ed8`): `ab2620` é o laço; `ab1960` lê o evento (1 = um botão do
`uikit` decidido); `ab1af0` → `ab1e80` traduz o **id do botão** (`[evento+4]`, do `uikit_setting_battle_command_00.bin`)
na ação gravada em `[[...]+0x78]+0x10`: id 1 → 1 (Lutar), 2 → 2, 3 → 3, 4 → 4 (Fugir), 5 → info, 6 → parceiro, 7 → 0xA.
Um id novo cai no caso vazio. O layout (`bin/app/battle/bin/battle_command_00*.arc`, SARC com BFLYT e até o XML-fonte)
tem os painéis `N_command_00..03`/`L_command_00..03` e as animações `command_patern_00/01`; o `layout_setting` e o
`uikit_setting` são FlatBuffers que ligam painéis e ids por hash.

**A decisão da bola** (`7dcb00`, o "juiz" do arremesso): `b = 65536 / (255/a)^(3/16)`, quatro checagens com o xorshift da
cena (`[[+8]+0x720/0x728]`), as que passaram em `[juiz+0x128]` (4 = pegou; Master Ball e o caso `[+0x748] == 3` forçam 4).
`7dd230` decide o próximo estado (0 sacudidas → 6, senão 1). A cena toca as sequências por índice (`80f510(cena+0x238, n)`;
13/14 = o Pokémon fugindo, normal ou lendário).

**A saída da cena sem pegar**: `919970` ("acabaram as bolas": a mochila não tem os itens 4, 12, 5, 3, 2 nem 1, via `1c9750`)
leva a cena à fala 7 do `poke_get` ("You don't have a single Poké Ball left! You'll have to withdraw for now...") e ao fim
nativo. O "Run away" do menu da cena também termina ("Do you want to give up catching this Pokémon?" → Yes).

**As falas** (`bin/message/<idioma>/common/*.dat`, texto GF cifrado: chave `0x7C89 + i*0x2983` por linha, rotação de 3 bits
por caractere): `btl_std` linha 82 "You got away safely!", `poke_get` linhas 1 ("Oh! You encountered a wild X!"),
2 ("Now's your chance! Catch the wild X!", a dos lendários), 7 (sem bolas), 8–11 (escapou, conforme as sacudidas),
13 (fugiu), 14 ("You got away safely!"). Toda linha passa por `6786b0` (x0 = dados do arquivo, `[+0x90]`+2 = número
de linhas, `[+0x98]` a tabela de linhas, 8 bytes a partir de +4; x1 = a linha cifrada, w2 = tamanho, w3 = índice, x4 = o
buffer: texto u16 em `+0x58`, capacidade `+0x78`, tamanho `+0x7a`). `btl_std` tem 392 linhas e `poke_get` 70 em todos
os idiomas. O objeto que chega lá não é o do carregamento (`1afc00`).

**O 5º botão (CATCH), nativo.** Como o menu monta os botões (tudo visto em execução, Eevee e Pikachu):
- O `.arc` do idioma (`battle_command_00{,_eng,...}.arc`, SARC) tem o layout do menu (`blyt/battle_command_00.bflyt`) e o
  do botão (`blyt/battle_command_button_00.bflyt`, uma "parte" usada por `L_command_00..03`). A aparência de cada botão vem
  da animação `patern_00` do botão: quadros 0 lutar, 1 Pokémon, 2 mochila, 3 fugir, 4 voltar (este só no modo de 2
  jogadores). Ela troca as texturas (círculo `battle_command_*_off_00` BC3 100×100, ícone `battle_button_command_icon_0N`
  BC4 200×200, rótulos `grpfont_battle_*Button_text_00` BC5 e `_text_01` BC4 160×128: o texto é imagem, uma por idioma) e
  as cores dos materiais (brilho e camadas de fundo: cor branca do material; sombra do rótulo: cor preta; no `mat1`,
  `+0x24`/`+0x28` depois do nome).
- O código (`ab2730` → `ab2e60` para cada uma das 4 vistas em `menu+0x288..0x2e8`) acha o botão do `uikit` por uma
  tabela fixa de 4 hashes (`0x7000f84c50`), aplica a animação de padrão (hashes fixos no código) e grava no botão o
  callback de decisão (`[botão+0x150..0x168]` = `ab49f0`/`ab4a20`/`ab4a30`/`ab4a50`, `+0x170` a vista, `+0x178` o evento):
  ao decidir, `{1, tabela 0x7000f98258[quadro da vista]}` → ids 1..4 e 7; `ab1e80` traduz o id em ação.
- Os 4 botões são posicionados pelo código a partir da borda direita da área `B_bounding_00` (RUN = borda − 66, os outros
  a cada 136); a grade de navegação do `uikit` tem o número de colunas no campo `f1` da tabela de navegação (4).
- Em batalhas de treinador o código esconde o RUN (`34e8b0` pelo hash) e fica com 3 botões.

O que o runtime faz (`runtime/hle/catch_button.c`, a partir dos arquivos do próprio jogador; `tools/catch_button.py` é o
mesmo em Python e `tools/catch_button_art.py` desenha a arte, embutida em `runtime/hle/catch_button_art.h`):
- texturas do botão VOLTAR redesenhadas como CATCH (mesmo tamanho e formato; compressão `stb_dxt`, swizzle do Tegra);
  rótulos CATCH / CAPTURE / Fangen / CATTURA / CAPTURAR / つかまえる / 잡는다 / 捕捉 (com "CATCH" pequeno nos asiáticos);
- `blyt/battle_command_button_catch.bflyt`: cópia do layout do botão apontando para essas texturas, em vermelho, sem a
  animação de padrão; no layout do menu, `N_command_04`/`L_command_04` no lugar do RUN, e os 4 botões e a área
  `B_bounding_00` um passo (136) à esquerda;
- `layout_setting`: os 20 painéis novos e as 6 animações do `L_command_04`; `uikit_setting`: o 5º botão (o do RUN com os
  hashes derivados: FNV-1a 64 de `"%016x:catch"`) e a 5ª coluna da grade (5 colunas). FlatBuffers só recebem acréscimos no
  fim (offsets apontam para frente: as listas que crescem são copiadas inteiras);
- `runtime/services/fs.c` registra os 11 arquivos com dois conteúdos: o montado, entregue enquanto uma batalha do modo
  roda (`wildbattle_menu_active`), e o original completado com zeros até o mesmo tamanho nas outras batalhas;
- `views_hook` (`ab2730`) grava no CATCH o mesmo callback com uma vista própria de quadro 5 (id 0, que o jogo ignora);
  `invoke_hook` (`ab49f0`) nota que foi o CATCH e `decide_hook` (`ab1e80`) transforma em pedido de captura.

**Implementado** (`runtime/hle/wildbattle.c`, "Catching during the battle"; testado no Eevee e no Pikachu, save
`build/save-edit`):
- CATCH → a batalha termina como fuga, que o estado 5 do evento troca por vitória → a captura nativa;
- vida, vida máxima e status do selvagem guardados no `9e4280` (id `+0x87` ≥ 6); a taxa da espécie que a fórmula pede
  (`71dfd0`, id 8) vira `taxa × (3·máx − 2·vida)/máx × (2,5 sono/congelado; 1,5 paralisia/veneno/queimadura)`, até 255.
  O anel da cena muda de cor junto (visto: vermelho com taxa 1);
- desmaiou (vitória com vida 0) → o resultado vira fuga → sem captura;
- uma bola por vez: depois de uma bola que não pegou (`7dcb00`), `919970` responde "sem bolas" → a cena se encerra
  sozinha → **a batalha de novo** (do laço do mundo, `2f78d0`, ainda com a tela escura), com a vida e os blocos de status
  postos de volta no Pokémon de batalha quando ele é montado (`9e2490`, BTL_POKEPARAM::Setup). Visto: Pidgey 3/17 →
  captura sem sucesso → revanche com a barra em 3/17 → desmaiou → sem captura;
- falas: "Time to catch it!" no lugar do "You got away safely!" da batalha; "Now's your chance! Catch the wild X!" (a
  linha 2 nativa) no lugar do "Oh! You encountered..."; "It's still raring to go! Back to the battle!" no lugar da fala
  de "sem bolas". Textos próprios nos 9 idiomas do jogo (`k_catch_text`).

**Ferramentas novas** (docs/DEPURACAO.md): entradas `b<N>:<botão>:<len>` e `c<N>:...` no `LARECOMP_INPUT` (a partir do início
da batalha / da captura) e `LARECOMP_FNPROF=b<de>:<até>`; `LARECOMP_WATCH_READ=1` vigia leituras; `LARECOMP_WILDBATTLE_*`:
`STATE_LOG`, `CATCHLOG`, `HPLOG`, `HPSCAN`, `MENULOG`, `MSGLOG`, `AUTOCATCH`, `FORCE_RATE`, `CATCH_MENU`, `CATCH_DUMP`.

**Ajustes depois do primeiro teste do usuário** (23/10/2026; detalhes no HISTORICO):
- posição: a área `B_bounding_00` fica onde está (com 5 botões o jogo já começa um passo antes): o CATCH termina onde o
  RUN terminava;
- sem relógio: `28c770` (montagem da batalha estática) grava 300 s em `+0x562` do evento; o gancho zera só nas nossas.
  O bit `0x80` de `+0x574` fica: sem ele o selvagem desmaia do jeito das batalhas de treinador, cuja sequência procura
  um modelo inexistente (`a9c700`);
- XP: `9d8ae0` (a checagem "XP ligada": competidor ≤ 1, setup `+0x4a4` zero, sem `0x80`/`0x800` em `+0x494`) responde sim
  nas nossas batalhas: o nocaute dá a XP nativa (fala 44/46 do `btl_std`, barra, subida de nível) e a batalha acaba;
- o "Run away" da captura: falas 24/4/14 do `poke_get` trocadas na nossa captura ("Back to battle", "Do you want to go back
  to the battle?", "Back to the battle!");
- a volta à batalha: `ee330.bseq` (a câmera de entrada do selvagem) de 1 quadro, entregue pelo `fs.c` só na volta
  (`wildbattle_quick_intro`), e a janela de "A wild X appeared!" não abre (`aaf230`); o envio (`ee420`, ou o parceiro
  do ombro) fica inteiro, mas escondido: fade-in segurado, sem ritmo de quadros e sem som até o menu (`ab2730`);
- "Back to battle" sem confirmação: o diálogo de saída da captura (`7d2380`) vai do estado 0 (`7de440`, a pergunta)
  direto ao 1 (`7de7d0`, a saída); o runtime aperta A na fala seguinte (`input_press_a_at`);
- texturas com mipmaps (o mod PT-BR): todos os níveis regravados;
- depois de uma bola que estoura, o selvagem age primeiro: o menu da volta é respondido escondido com uma fuga que falha
  (`9f7300` responde não; a fala 83 não abre), então o turno é só dele;
- chance de captura: o fator de vida/status vai no resultado da fórmula (`7de080`: `taxa^0,54 × ...`, até 255), não na
  taxa da espécie;
- CATCH com o Pokémon preso (Wrap etc.): a checagem de fuga do cliente (`a5c350`) responde sim;
- evolução: depois de um nocaute nosso, o pós-batalha (`319bc0`) vai do estado 0 ao 1 (o laço de evolução das batalhas de
  treinador: `728300` checa, `83cef0` abre o evento de evolução) em vez do 4 (fim).
Endereços novos no Pikachu: `28c770` → `28c740`, `9d8ae0` → `9d8af0`, `7de080` → `7de0b0`, `a5c350` → `a5c360`,
`aaf230` → `aaf240`, `319bc0` → `319b90`, `7de440` → `7de470`, `9f7300` → `9f7310`. Testado por roteiro nos dois jogos.

**O inicial andando sozinho** (24/10/2026, issues #6/#22; HISTORICO): também batalha. Nas nossas batalhas, com ele sozinho,
`a7e090` ("é o inicial?") responde não, então ele sai de uma Poké Ball, e `a99b40` (o Pokémon do modelo de batalha)
recebe a forma 0: o modelo da espécie em vez do modelo de campo de quem anda junto, que a batalha estática não tem.
Pikachu: `a7e090` → `a7e0a0`, `a99b40` → `a99b50`.

**Desmaio e música** (24/10/2026; HISTORICO): o bit `0x80` de `+0x574` sai quando o menu aparece, e o selvagem derrotado
desmaia como nas batalhas de treinador (`ee620`; `a9c700` responde "sem junta" a um modelo de chave 0). A música:
`16d060` (eventos do Wwise por nome) não manda o `Pop_BGM_battle_to_field` de uma captura que volta para a batalha nem o
`Push_BGM_field_to_battle` da volta. Pikachu: `a9c700` → `a9c710`, `16d060` → `16d030`.

**Falta**: com o mod de tradução PT-BR o CATCH aparece como um quadrado preto (HISTORICO); a espera de ~12 s entre escolher (CATCH ou o RUN do jogo, igual) e a batalha sair; natureza/habilidade
do Pokémon tocado (o shiny e o tamanho passam desde a issue #67); ver uma batalha de treinador de verdade com o menu original (testado só com `CATCH_MENU=0`); os
rótulos dentro do jogo em outros idiomas (conferidos decodificando os arquivos montados, `CATCH_DUMP`).

## Próximas etapas
_Atualizado em 17/10/2026: os itens 4 e "ligar/desligar no launcher" do 5 foram feitos (Etapa 4 acima, opção
"Batalhas selvagens (experimental)" desde a 0.2.0, nos dois jogos desde a 0.2.1). Falta o resto do 5 (XP dividido, fuga,
o Pokémon sumir do mapa) e o 3._

3. Confirmar o caminho alternativo (`289c4c` → `28bda0`, modo 3) e a captura pós-batalha (`319e20` → `31a0e0`) em
   execução: o ideal é um save perto de uma batalha selvagem nativa (lendário ou Snorlax); sem isso, forçar o
   método e observar.
4. **Protótipo**: no encontro selvagem (`305ad0`), em vez de `28ef70`, montar o pedido de batalha selvagem para o
   Pokémon tocado e iniciar a batalha; no fim, a captura.
5. Detalhes: fugir, experiência, o Pokémon sumir do mapa, ligar/desligar o modo no launcher.
