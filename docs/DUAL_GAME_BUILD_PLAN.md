# Plano de integração: Chains of Olympus + Ghost of Sparta

## Objetivo
Um único APK e uma única instalação, com seleção de jogo no launcher existente.

## Situação técnica verificada
- Chains of Olympus usa o ID `UCUS98653`.
- Ghost of Sparta usa o ID `UCUS98737`.
- O workflow de Ghost of Sparta compila uma única `libmain.so` para `GAME=UCUS98737_full`.
- `GameActivity.getLibraries()` atualmente retorna `SDL3` e `main`.
- O launcher atual guarda um único par `game.iso` / `image.bin`; isso não separa dois títulos.
- A validação isolada do launcher usa um runtime stub e não representa um APK jogável.

## Ordem de implementação
1. **Separar dados por título**: diretórios individuais e preferências de jogo selecionado; não sobrescrever os arquivos do jogo atual.
2. **Confirmar carregamento dinâmico SDL**: determinar em que momento SDLActivity chama getLibraries e como o nome da biblioteca é associado ao ponto de entrada nativo.
3. **Build nativa dupla**: compilar cada ID separadamente e empacotar bibliotecas com nomes exclusivos, sem colisão de `libmain.so`.
4. **Launcher**: tornar os dois títulos selecionáveis e direcionar o botão Jogar para o título escolhido.
5. **Empacotamento**: produzir um único APK assinado com ambas as bibliotecas e recursos necessários.
6. **Validação**: inspecionar conteúdo do APK, verificar assinatura, instalar e testar inicialização de cada título. Só declarar pronto após teste real dos dois.

## Bloqueio conhecido
O workflow antigo de Ghost of Sparta depende do secret `GOW_GOS_DISC_URL`; o download pelo Google Drive falhou anteriormente. A build principal não é prova de que Ghost of Sparta está integrado.

## Regra de segurança da build
Não reutilizar uma biblioteca compilada para um ID como substituta da outra. Não publicar o APK de validação com stub como jogável.
