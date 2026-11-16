# Contribuindo

Obrigado por ajudar no port do Mini vMac para PlayStation 2!

## Issues

- Pesquise issues existentes e abra uma issue por problema.
- Informe se testou em PCSX2 ou PS2 real; inclua modelo/revisão do console quando souber.
- Descreva versões do port, ps2sdk e gsKit, passos para reproduzir e resultado esperado/observado.
- Para congelamentos, informe os valores de status `T` e `PC` e mensagens relevantes do `BOOTLOG.TXT`.
- Não anexe ROM, BIOS, disco/imagem de sistema, jogo comercial ou outro arquivo protegido. Informe somente formato e tamanho da imagem de teste.
- Remova caminhos pessoais e informações privadas dos logs.

## Pull requests

- Faça mudanças focadas, explique a motivação e descreva como verificou.
- Rode `make` quando a toolchain PS2 estiver disponível. Diferencie resultado de build de teste em runtime.
- Não inclua executáveis, objetos, logs, ROMs nem imagens de disco.
- Mantenha a integração PS2 em `src/` e `cfg/`; o core Mini vMac fica em `vendor/minivmac/`.
- As contribuições precisam ser compatíveis com GPL v2.
