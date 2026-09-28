# OBS Multichat

Primeiro protótipo do plugin para OBS Studio, baseado no template oficial do OBS.

## Estado atual

O plugin registra um painel acoplável em Exibir → Painéis. Permite cadastrar
endereços de YouTube, Twitch, Kick e TikTok, salvar os campos e mostrar duas
mensagens de demonstração pelo botão Testar painel.

Após salvar um canal da Twitch, esta versão tenta receber mensagens reais
diretamente por IRC/TLS, sem popup e sem login. O canal também reconecta
automaticamente após uma queda de conexão. O conector experimental da Kick busca o ID da sala no endereço público do canal
e se inscreve no fluxo de mensagens. A Kick pode impedir essa consulta com
verificação no navegador; o painel informa a falha. O conector experimental
do YouTube aceita a URL de uma live ou do chat, lê o identificador da live
e consulta as mensagens sem login, respeitando o tempo sugerido pela própria
plataforma. A interface usada pelo YouTube não é pública e pode mudar. TikTok
ainda está pendente. Ao marcar Exibir na transmissão, o plugin cria uma fonte
de navegador Zosma Multichat Web na cena atual e nas cenas usadas depois.
Ela recebe as mensagens de um servidor HTTP acessível somente em 127.0.0.1,
dentro do plugin, e mostra até o número de linhas escolhido no painel. O grupo
Aparência oferece tamanho de letra, estilo cartões ou simples, cores dos nicks
por plataforma e cor do fundo da transmissão (inclusive transparente), além
da cor de fundo do painel de mensagens. As escolhas ficam salvas e atualizam
a fonte de navegador em tempo real.
O OBS salva a fonte nas cenas e o plugin atualiza seu endereço local ao reiniciar.
A fonte de texto anterior Zosma Multichat é removida na primeira ativação
da fonte de navegador. O painel usa apenas o símbolo da plataforma em cada
mensagem. As badges da Twitch são identificadas pelas tags IRC e consultadas
pela API pública UNTTV (unttv.vercel.app), incluindo as específicas do canal;
o carregamento dessas badges depende da disponibilidade desse serviço. Emotes da
Twitch e da Kick aparecem como imagens na transmissão e como imagem estática
no painel quando carregam. Badges da Kick são mostradas pelo nome; quando o
evento inclui uma URL de imagem válida, essa imagem é usada. A API pública de
mensagens da Kick não fornece as imagens das badges personalizadas do canal.
O plugin baixa as
imagens com libcurl e as fornece à fonte de navegador pelo servidor local;
o painel indica quantas imagens carregaram e quantas ficaram indisponíveis.

Quando o Qt no OBS não disponibiliza TLS, a conexão de leitura pública da
Twitch usa o endpoint IRC sem TLS na porta 6667. O painel informa quando
isso ocorre. Nenhuma senha ou token é enviada nessa conexão.

## Primeiro teste

1. Em Actions, baixe o artefato windows-x64 do build mais recente.
2. Extraia o pacote e copie as pastas do plugin para a pasta correspondente do
   OBS Studio portátil, preservando a estrutura do arquivo.
3. Reinicie o OBS, abra o painel Multichat e clique em Testar painel.
4. Salve uma fonte, reinicie o OBS e confirme que o campo continua preenchido.

O código é compilado separadamente para Windows, macOS e Linux. O teste inicial
usa o pacote extraível para facilitar a substituição dos arquivos.

## Próximas etapas

Implementar a captura do TikTok e opções adicionais de aparência para o chat.

Este projeto inclui código e infraestrutura derivados do template oficial
https://github.com/obsproject/obs-plugintemplate sob a licença em LICENSE.
Não contém código do Social Stream Ninja.
