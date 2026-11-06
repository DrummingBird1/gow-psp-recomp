# Android Dual APK — status e processo de release

## Estado atual

- [x] Chains of Olympus integrado (ID `UCUS98653`).
- [x] Ghost of Sparta integrado (ID `UCUS98737`).
- [x] Bibliotecas nativas separadas no mesmo APK: `libmain_coo.so` e `libmain_gos.so`.
- [x] Dados de cada jogo separados pelo ID em `gow-games/<ID>/`.
- [x] Workflow Dual compila os dois runtimes, instala os recursos do launcher e empacota um APK.
- [x] CI verificou assinatura e presença das duas bibliotecas no APK.
- [x] Usuário confirmou teste real dos dois jogos funcionando no mesmo aplicativo.

A integração Dual está funcional no dispositivo testado. Isso não equivale a uma certificação para todos os aparelhos Android.

## Workflow oficial de build

O workflow principal é [Build Android — Dual God of War PSP](../.github/workflows/android-dual-game.yml).

Ele:
1. Obtém as imagens de disco configuradas nos secrets privados `GOW_DISC_URL` e `GOW_GOS_DISC_URL`.
2. Confere que cada imagem corresponde ao ID esperado durante a extração.
3. Decifra e recompila cada EBOOT separadamente.
4. Compila SDL3 uma vez e compila um runtime ARM64 para cada jogo.
5. Gera o projeto Android com o launcher e inclui os recursos correspondentes.
6. Empacota e assina um APK com as duas bibliotecas.
7. Verifica a assinatura e exige que as duas bibliotecas estejam presentes.
8. Publica somente um artefato temporário do Actions.

**Segurança:** ISOs e dados proprietários não devem ser adicionados ao Git, aos releases ou aos artefatos distribuídos. Os links usados no CI são secrets privados; o APK distribuído deve conter apenas o que é necessário e permitido pelo projeto. Nunca usar a biblioteca de um jogo como substituta da outra.

## Workflows auxiliares

- `android-gow.yml`: build legado de Chains of Olympus, apenas manual.
- `build-ghost-of-sparta.yml`: build isolado de diagnóstico, apenas manual; não é o APK Dual.
- `android-fiber-check.yml`: verificação de compilação de fibras ARM64, acionada por mudanças nos arquivos relevantes do runtime.
- `android-sdl.yml`: build manual auxiliar do SDL3.

Mudanças normais no launcher/runtime devem priorizar o workflow Dual. Os workflows individuais ficam disponíveis para diagnóstico explícito e não devem disparar em paralelo a cada alteração do launcher.

## Checklist antes de publicar uma release Dual

- [x] Os dois jogos testados pelo usuário no mesmo app.
- [x] Build Dual de CI concluída com sucesso anteriormente.
- [ ] Executar novamente o workflow Dual após as mudanças de documentação/workflow.
- [ ] Confirmar que o APK final contém as duas bibliotecas e passa na verificação de assinatura.
- [ ] Confirmar que o APK não contém ISOs, EBOOTs ou outros arquivos de disco distribuíveis.
- [ ] Baixar o APK do artefato e fazer uma instalação/atualização final no aparelho.
- [ ] Criar uma release versionada com o APK Dual validado, notas PT/EN e nome de arquivo claro.
- [ ] Atualizar a constante `VERSION` do launcher para corresponder exatamente à tag publicada.

## Nome recomendado do APK

`GodOfWar-PSP-Recomp-Dual.apk`

## Observação sobre o atualizador

O atualizador do app consulta `/releases/latest`, não os artefatos do Actions. Portanto, um build bem-sucedido no Actions não deve ser anunciado como uma release publicada. A versão exibida no launcher só deve ser incrementada junto com uma release real.
