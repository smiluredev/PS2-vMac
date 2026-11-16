# PS2-vMac

Port do [Mini vMac](https://github.com/minivmac/minivmac) para PlayStation 2, com vídeo via `gsKit`, áudio via `audsrv` e alvo Macintosh Plus. O código é aberto para colaboração; veja [CONTRIBUTING.md](CONTRIBUTING.md).

> O port está em desenvolvimento. Os testes dependem de PCSX2 ou de um PS2 real; uma compilação bem-sucedida não garante compatibilidade com todas as combinações de console, periféricos e imagens.

## Recursos

- Renderização do Macintosh Plus usando `gsKit`.
- Áudio via `audsrv` (PCM estéreo de 16 bits).
- Entrada por DualShock 2, teclado USB e mouse USB compatíveis com os módulos do PS2SDK.
- Carregamento de ROM e imagens de disco a partir de USB, memory card ou `host:` no PCSX2.
- Leitura sob demanda para imagens grandes, com cache para reduzir acessos ao USB.
- Módulos IOP necessários embutidos no ELF durante a build.

## Compilar

É necessário ter `ps2sdk`, `gsKit`, `make` e os compiladores da toolchain `ps2dev` disponíveis.

O caminho padrão é `/usr/local/ps2dev`. Para usar outros caminhos:

```sh
make
# Exemplo:
make PS2DEV=/opt/ps2dev PS2SDK=/opt/ps2dev/ps2sdk GSKIT=/opt/ps2dev/gsKit
```

O executável é `minivmac.elf`. `make clean` remove os artefatos gerados pela build.

## Arquivos necessários para executar

Este repositório **não distribui** ROM de Macintosh nem imagens de sistema/discos. Use somente arquivos que você tenha direito de usar. Coloque-os em um dos locais procurados pelo port, por exemplo:

```text
mass:/vMac.ROM
mass:/disk1.dsk
mass:/minivmac/vMac.ROM
mass:/minivmac/disk1.dsk
mc0:/minivmac/
host:/minivmac/     (PCSX2, com host filesystem configurado)
```

São reconhecidos nomes de ROM como `vMac.ROM` e `Mac-Plus.ROM`. O port procura discos `disk1` a `disk9` e alguns nomes de disco de boot, com extensões `.dsk`, `.img`, `.image` e `.hfv`. O driver BDM USB espera um formato de partição compatível com a configuração do PS2 (normalmente FAT32).

**Não publique ROMs, BIOS, dumps protegidos, imagens comerciais/de sistema ou arquivos pessoais** em issues, pull requests ou releases. Não anexe esses arquivos ao pedir ajuda.

## Controles

- Analógico esquerdo: mover o cursor; D-pad: ajuste fino de um pixel.
- Cruz: clique; círculo: Return; quadrado: Backspace.
- Triângulo: Command; Start: Control Mode; R1: Option; L2: Shift.
- R2: alternar escala de vídeo; R3: alternar fast-forward.
- Teclado e mouse USB dependem do dispositivo e do suporte dos módulos IOP/PS2SDK.

## Código e licença

A camada do PS2 está em `src/` e `cfg/`. O código-fonte necessário do Mini vMac está em [`vendor/minivmac/src`](vendor/minivmac/src), com a licença original em [`vendor/minivmac/COPYING.txt`](vendor/minivmac/COPYING.txt). O projeto é distribuído sob GPL v2; consulte [`LICENSE`](LICENSE).
