# Compilar e jogar no Windows

Passo a passo do zero até o jogo aberto, a partir do código-fonte. **Para só jogar, use a release** (o zip em
Releases, veja o [README](../README.md#how-to-play)): ela não precisa de nada disto.

Leva por volta de meia hora, quase toda esperando downloads e a compilação.

## O que você precisa
- Windows 10 ou 11, 64 bits
- CPU com SSE4.2 (Intel a partir de 2008 / Nehalem, AMD a partir de 2011 / Bulldozer); com AVX2 (Intel a partir de
  2013 / Haswell, AMD a partir de 2015 / Excavator) o código do jogo roda mais rápido. O `make` compila o código do
  jogo para `GENARCH` (padrão `-march=x86-64-v3`; `GENARCH=-march=x86-64-v2` para uma CPU sem AVX2, apagando
  `build/obj/` antes: os objetos não dependem das flags); a tela inicial escolhe sozinha pela CPU
- GPU com Vulkan 1.3 e driver atualizado (NVIDIA, AMD ou Intel recentes)
- Uns 10 GB livres (arquivos do jogo, código gerado e build)
- [Git for Windows](https://git-scm.com/download/win) (traz o Git Bash)
- **Os seus próprios arquivos do jogo**: *Pokémon: Let's Go, Eevee!* ou *Pikachu!*, extraídos da sua cópia (o Eevee
  **v1.0.0** é o mais testado; as opções experimentais pedem a 1.0.0 de um dos dois ou o Eevee 1.0.2). Este
  projeto não fornece nem ensina a obter esses arquivos ou as chaves.

## 1. Baixar o projeto
No Git Bash, numa pasta com espaço livre (de preferência fora de `Arquivos de Programas`):

```bash
git clone https://github.com/raulzrrs/letsgo-recomp.git
```

## 2. Instalar as ferramentas (uma vez)
No PowerShell, dentro da pasta do projeto:

```powershell
powershell -ExecutionPolicy Bypass -File tools\setup_windows.ps1
```

O script instala:
- o **MSYS2** em `C:\msys64`, com clang, make, SDL3, FFmpeg, Vulkan e shaderc;
- o **Python 3.12**, com o ambiente do gerador de código em `.venv`.

Ele também configura o clone para fim de linha LF. Pode rodar de novo se algo falhar no meio.

## 3. Compilar o runtime
Abra o **Git Bash** na pasta do projeto e coloque o MSYS2 no PATH (vale para essa janela):

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
make -j20 build/lasrecomp.exe
```

Isso gera `build/lasrecomp.exe`, o runtime (o launcher e tudo o que substitui o Switch). Mudanças no runtime
recompilam em segundos.

## 4. Montar o jogo pelo launcher (o caminho mais simples)

```bash
./build/lasrecomp.exe
```

No launcher, clique em **Selecionar pasta do jogo...** e escolha a pasta com o jogo extraído (a que tem `exefs` e
`romfs`, ou um `romfs.bin`). Ele confere se é o Eevee ou o Pikachu (program id), copia os arquivos para `extracted/`, gera o código do jogo
com o `.venv` e o compila com o clang do MSYS2 (`extracted/lasgame.dll`). É o mesmo fluxo da release, com as
ferramentas de desenvolvimento no lugar do gerador congelado e do Zig. Depois, clique em **Jogar**.

## 4b. Ou à mão
Crie a pasta `extracted/` na raiz do projeto com:

```
extracted/
  exefs/      main, main.npdm, rtld, sdk, subsdk0, subsdk1   (o ExeFS do jogo)
  romfs.bin   a imagem RomFS do jogo (uns 4,4 GB)
  romfs/      opcional: o RomFS extraído em pastas (útil para mods; o jogo lê o romfs.bin)
```

O código gerado depende do executável exato: os remendos do gerador (`tools/patches.py`) e os ganchos do runtime só
são endereços do `main` (tabela por build: Eevee 1.0.0 e 1.0.2, Pikachu 1.0.0, `tools/addrmap.py`) e ficam desligados
para qualquer outro. `extracted/` está
no `.gitignore` e nunca deve ir para o git.

Traduza o código do jogo para C (o resultado vai para `generated/`, fora do git) e compile. **Sempre com `-j`**: sem
ele, os ~920 arquivos do jogo compilam um por vez e passam de uma hora.

```bash
make gen
make -j20
```

Isso gera também `build/lasgame.dll` (o código do jogo), em 5 a 20 minutos conforme a CPU.

## 5. Jogar

```bash
./build/lasrecomp.exe
```

- Os saves ficam em `save/`, as configurações em `larecomp.ini` e os caches de shader em `cache/`, tudo na pasta do
  projeto.
- `--play` pula o launcher: `./build/lasrecomp.exe extracted --play`.
- `make jogo` monta em `build/jogo/` uma cópia que abre com dois cliques (traz as DLLs do MSYS2) e deixa recompilar
  `build/` enquanto se joga.
- A primeira vez em cada área compila os shaders. Pode haver objetos aparecendo atrasados por um instante; da segunda
  vez em diante fica liso.

## 6. A release (o zip para quem só quer jogar)

```bash
make deps-windows          # FFmpeg só LGPL, com os decodificadores que o jogo usa (precisa do nasm do MSYS2)
make gen-helper            # o gerador congelado com o PyInstaller (sem Python no PC do jogador)
tools/fetch_zig.sh         # o compilador Zig, versão fixa e conferida por sha256
make dist                  # build/dist/LetsGoRecomp-<versão>-windows.zip
```

O zip traz o `LetsGoRecomp.exe`, as DLLs, o gerador, o Zig enxuto (`tools/zig_slim.sh`) e os cabeçalhos do código do
jogo. Nada do jogo vai junto: o código é gerado no PC do jogador, a partir da cópia dele. A versão vem de
`RT_APP_VERSION` em `runtime/paths.h`.

## Opcional
- **Tradução / mods:** no launcher, página **Mods**, **Instalar mod...** e escolha o arquivo baixado (.zip, .rar, .7z);
  ele já vem marcado na lista (dá para usar vários juntos; se dois trocam o mesmo arquivo, vale o de cima). Também dá para copiar o `romfs` do mod para `mods/<nome>/romfs` à mão (**Abrir a
  pasta de mods**).
- **Fonte do sistema do Switch:** veja [fonts/README.md](../fonts/README.md).
- **Melhor imagem:** com uma GPU folgada, escolha uma resolução acima da sua tela (por exemplo 4K numa tela 1080p):
  o jogo é renderizado maior e reduzido, o que elimina quase todo o serrilhado.

## Problemas comuns
| Sintoma | O que fazer |
|---|---|
| "avutil-…dll / SDL3.dll não encontrado" ao abrir o exe | rode pelo Git Bash com o MSYS2 no PATH, ou use a cópia de `make jogo` (traz as DLLs) |
| o launcher pede para montar o jogo de novo | o gerador ou os cabeçalhos mudaram: escolha a pasta de novo (ou, à mão, `make gen` e `make -j20`) |
| `make gen` falha | confira o `.venv` (rode de novo o `setup_windows.ps1`) e se `extracted/exefs/main` existe |
| tela preta ou erro de Vulkan | atualize o driver da GPU; precisa de Vulkan 1.3 |
| o jogo fecha sozinho | mande a saída do terminal (ou o `lasrecomp.log` da release) num issue (veja [CONTRIBUTING.md](../CONTRIBUTING.md)) |

## Para quem vai mexer no código
- [DEPURACAO.md](DEPURACAO.md): variáveis de diagnóstico e o `tools/repro.sh`, que roda o jogo com entrada roteirizada e
  tira prints em quadros escolhidos.
- [HISTORICO.md](HISTORICO.md): como cada problema foi resolvido. É a melhor forma de entender o runtime.
- Mudou `runtime/cpu.h` ou `runtime/gamelib.h`? A DLL do jogo precisa ser recompilada (`make -j20`).
