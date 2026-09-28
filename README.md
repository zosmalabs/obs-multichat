# OBS Multichat

Primeiro protótipo do plugin para OBS Studio, baseado no template oficial do OBS.

## Estado atual

O plugin registra um painel acoplável em Exibir → Painéis. Permite cadastrar
endereços de YouTube, Twitch, Kick e TikTok, salvar os campos e mostrar duas
mensagens de demonstração pelo botão Testar painel.

Esta versão ainda não recebe mensagens reais, não abre páginas de captura e
não coloca chat na transmissão. Ela valida carregamento, painel e persistência.

## Primeiro teste

1. Em Actions, baixe o artefato windows-x64 do build mais recente.
2. Extraia o pacote e copie as pastas do plugin para a pasta correspondente do
   OBS Studio portátil, preservando a estrutura do arquivo.
3. Reinicie o OBS, abra o painel Multichat e clique em Testar painel.
4. Salve uma fonte, reinicie o OBS e confirme que o campo continua preenchido.

O código é compilado separadamente para Windows, macOS e Linux. O teste inicial
usa o pacote extraível para facilitar a substituição dos arquivos.

## Próximas etapas

Implementar captura em segundo plano, reconexão, mensagens reais, identidade
visual e fonte de cena. Entradas previstas: Twitch e Kick (@canal ou URL do
chat), YouTube (URL da live ou chat), TikTok (@usuário ou URL da live).

Este projeto inclui código e infraestrutura derivados do template oficial
https://github.com/obsproject/obs-plugintemplate sob a licença em LICENSE.
Não contém código do Social Stream Ninja.
