# Compilar e rodar no Linux (nativo)

A build de Linux roda o jogo como um binário ELF nativo (sem Wine, sem Proton). Ela foi compilada e jogada no Fedora 45,
x86-64, AMD RADV (iGPU RDNA2), Vulkan 1.4. A build de Windows não é afetada por nada daqui.

## O que é preciso

| | |
|---|---|
| GPU | Vulkan 1.3 |
| CPU | x86-64 com SSE4.2; AVX2 recomendado (o código do jogo é compilado para x86-64-v3) |
| Disco | cerca de 5 GB para a biblioteca do jogo, mais cerca de 5 GB para a imagem RomFS montada na primeira configuração |
| Bibliotecas | SDL3, FFmpeg 8 (`libavformat`, `libavcodec`, `libavutil`, `libswscale`), `libvulkan`, shaderc (`libshaderc` ou `libshaderc_shared`) |

Fedora 45:

```bash
sudo dnf install clang lld make git python3 SDL3-devel ffmpeg-free-devel \
    vulkan-loader-devel vulkan-headers libshaderc-devel dejavu-sans-fonts

# o Fedora não tem pacote do VulkanMemoryAllocator: põe o header onde a build o encontra
sudo mkdir -p /usr/include/vma
sudo curl -L -o /usr/include/vma/vk_mem_alloc.h \
    https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.5.0/include/vk_mem_alloc.h
```

Debian / Ubuntu: os mesmos pacotes (`libsdl3-dev`, `libavformat-dev`, ..., `libvulkan-memory-allocator-dev`,
`fonts-dejavu-core`). O `vulkan-memory-allocator-dev` já entrega o `<vk_mem_alloc.h>`.

As flags do compilador e do linker vêm do pkg-config (`sdl3`, `libavformat`, `libavcodec`, `libavutil`, `libswscale`,
`vulkan`, `shaderc`), então uma distribuição que guarda os headers do FFmpeg num subdiretório (Fedora:
`/usr/include/ffmpeg`) não precisa de flag extra.

## Compilar

```bash
python3 -m venv .venv
.venv/bin/pip install unicorn capstone lz4    # o gerador (tools/gen.py)
make gen                                      # extracted/exefs  ->  generated/
make -j$(nproc)                               # build/lasrecomp e build/lasgame.so
```

O `make gen` traduz os seus próprios arquivos do jogo (`extracted/exefs`) para C. Leva cerca de um minuto por módulo. A
release de Windows leva o mesmo gerador congelado como `lasgen`; no Linux ele roda do `.venv`.

O código do jogo é compilado para o nível de CPU desta máquina: x86-64-v3 (AVX2) onde o `/proc/cpuinfo` o tem, x86-64-v2
caso contrário (`make GENARCH=-march=x86-64-v3` força um). `make CC=... CXX=...` e `LARECOMP_CC` escolhem outro
compilador.

O `make -j` compila duas coisas: `build/lasrecomp` (o runtime: janela, entrada, renderizador Vulkan, serviços do
sistema) e `build/lasgame.so` (o código do jogo, ~900 arquivos C, cerca de 35 minutos em 16 threads). Só a primeira
build leva esse tempo; mudar arquivos do runtime recompila só eles.

## Rodar

```bash
./build/lasrecomp                      # a tela inicial: escolha ali a pasta do jogo
./build/lasrecomp extracted --play     # direto no jogo
```

A pasta do jogo é a que tem `exefs/` e `romfs/`. A primeira configuração copia (por hard link, onde o sistema de
arquivos permite) os arquivos do jogo para a raiz de dados e escreve o `romfs.bin` a partir de `romfs/`: cerca de 4,5 GB,
alguns minutos. Saves, opções (`larecomp.ini`), caches de shader e mods ficam na raiz de dados.

Para rodar sem a árvore de código, copie estes três arquivos para uma pasta e inicie o binário por ali:

```bash
cp build/lasrecomp build/lasgame.so build/lasgame.so.id ~/letsgo/
~/letsgo/lasrecomp ~/lets_go_eevee --play
```

O `lasgame.so.id` é o que diz ao runtime que a biblioteca foi compilada com o mesmo gerador e as mesmas flags; sem ele
(ou com outro id) a tela inicial pede para recompilar o código do jogo. Essa recompilação funciona a partir de um
checkout do código: ela roda o `.venv/bin/python tools/gen.py` e o `/usr/bin/clang` (ou o `LARECOMP_CC`, ou o Zig da
release no Windows).

## AppImage

```bash
make appimage        # tools/make_appimage.sh: build/LetsGoRecomp-<versão>-x86_64.AppImage
```

Ele carrega o runtime, o `lasgame.so`, a fonte da tela inicial e as bibliotecas SDL3 / FFmpeg / shaderc que o binário
liga. O loader e o driver do Vulkan continuam os do sistema (como o `vulkan-1.dll` da build de Windows), e a fonte
compartilhada também. A pasta do próprio AppImage é somente leitura, então saves, opções, mods e os arquivos do jogo vão
para `$XDG_DATA_HOME/lasrecomp`. A portabilidade é a libc da máquina que o monta: um feito no Fedora 45 precisa da
glibc do Fedora 45 ou mais nova.

## Steam

O `make steam` escreve `~/.local/share/applications/letsgo-recomp.desktop` com `--play` nele e imprime os dois cliques
que faltam no Steam (*Jogos → Adicionar um jogo não Steam… → Procurar* até o binário). A tela inicial fica então só
para a configuração, e o botão Jogar do Steam inicia o jogo. `tools/steam_shortcut.sh --remove` apaga o arquivo da
entrada.

O `--play` precisa da pasta do jogo que a tela inicial configurou na raiz de dados (`extracted/`), e é por isso que a
entrada não leva caminho: ele não fica velho. Um AppImage pode ser adicionado ao Steam do mesmo jeito; dê a ele a opção
de inicialização `--play`.

## Notas do Linux

- **O upscaling FSR 3.1 é só do Windows** (a AMD entrega esse backend Vulkan como DLL, `amd_fidelityfx_vk.dll`). Onde a
  DLL não está, a escolha de anti-aliasing lê **FSR 1 (upscale)** no lugar de **FSR 3.1** e faz o que diz; os presets e
  o sharpening do FSR 3.1 não são oferecidos ali. SMAA, TAA, SSAO, GI, FSR 1 e a geração de quadros própria (2x / 3x /
  4x) funcionam.
- **Mods** instalados de um arquivo baixado (página Mods da tela inicial) são desempacotados com o `tar` do libarchive.
  O Windows o tem como `tar.exe`; no Linux instale o `bsdtar` (Fedora: `bsdtar`, Debian / Ubuntu: `libarchive-tools`).
  Um `tar` comum é usado quando falta o `bsdtar`, e ele lê `.tar` / `.tgz`, mas não `.zip` / `.rar` / `.7z`.
  `LARECOMP_TAR=<programa>` escolhe outro.
- **Huge pages:** a janela do guest é reservada com `MADV_HUGEPAGE`. A memória global dos shaders é o heap do jogo
  importado para a GPU, e o driver amdgpu repercorre toda essa faixa de userptr a cada submissão de comando: 28 ms por
  submissão com páginas de 4 KB (16,5 fps em vez de 30, e o áudio da abertura sumia a cada 170 ms), 2,2 ms com páginas
  de 2 MB. O `/proc/<pid>/smaps` mostra os 3,3 GB em `AnonHugePages`; sem isso, comece a investigar por aqui.
- **Geração de quadros** anda sem a grade do vertical blank do monitor (essa marcação é do compositor do Windows): cada
  imagem aparece quando é devida, o que num painel de 240 Hz pode espaçar duas delas um refresh em vez de dois.
- **Fontes:** o motor de fontes do jogo precisa de uma fonte TrueType no lugar das fontes do sistema do console. Na
  ordem: `LARECOMP_FONT=<arquivo>`, um dump em `fonts/` na raiz de dados, depois os caminhos comuns do sistema (DejaVu
  Sans, Noto Sans, Liberation Sans, Hiragino, Yu Gothic). Sem nenhuma delas o jogo para no boot com
  `FATAL: no usable system font substitute`. Não aponte para a Noto Sans CJK: são contornos CFF e o motor de fontes do
  jogo cai com elas.
- **Áudio, controles, teclado e mouse** passam pelo SDL3, então PipeWire / PulseAudio e evdev / udev funcionam como em
  qualquer outro jogo SDL3. `LARECOMP_NO_AUDIO=1` silencia.
- **Vulkan por software (llvmpipe) não apresenta num display Xvfb**: a execução para com `FATAL: no graphics+present
  queue`. Use um display de verdade, ou uma GPU de verdade com Xvfb.
- **Imagens para relatos de bug**: `LARECOMP_LAUNCHER_SHOT=<quadro>` grava `build/launcher.png` e sai;
  `LARECOMP_WINDOW_SHOT=<quadro> LARECOMP_EXIT_AFTER_SHOT=1` grava `build/window.png`. Entrada roteirizada:
  `LARECOMP_INPUT="<quadro>:<BOTAO>:<quadros>,..."` com `LARECOMP_NO_USER_INPUT=1`.
- Os logs de crash vão para `lasrecomp.log` na raiz de dados, o mesmo do Windows (`docs/DEPURACAO.md`).
