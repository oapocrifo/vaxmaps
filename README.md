# Vax Maps

Canvas infinito para mapas de RPG: vários mapas grandes na mesma página, desenho livre, textos e notas, camadas, zonas e régua calibrada pela grade do mapa. C++17 + Qt 6 Widgets.

## Uso

A janela não tem barras nem painéis fixos. Botão direito no canvas abre o menu com todas as ferramentas e comandos. Botão direito sobre um item selecionado (zona, mapa, texto, nota, traço) abre as propriedades dele.

| Tecla | Ferramenta |
|---|---|
| V | Selecionar e mover (Del exclui) |
| H, botão do meio, Espaço + arrastar | Mover a vista |
| Roda do mouse | Zoom no cursor |
| P / E | Caneta / Borracha |
| T / N | Texto / Nota (duplo clique edita) |
| M | Régua |
| K | Calibrar mapa (F detecta a grade, setas ajustam, Enter aplica, Esc cancela) |
| R / O / G | Zona retângulo / círculo / vértices (Shift = quadrado/círculo) |
| S | Snap na grade do mapa calibrado |
| L | Mostrar ou esconder o painel de camadas |
| F11 | Tela cheia |
| Esc | Fecha o popup aberto |

- Importar mapas: arraste arquivos para a janela ou Ctrl+I. Formatos: PNG, JPG, WebP, TIFF, BMP, GIF.
- Calibrar (como no Atlas VTT): selecione o mapa e tecle K (ou tecle K e clique no mapa). Clique num canto de um quadrado no quarto destacado e depois no canto seguinte à direita. O programa ajusta a grade às linhas do mapa; se as linhas não confirmarem, pede outra medição no próximo quarto (até 4). F procura a grade sozinho. A grade amarela é a prévia: setas a movem (Shift = 0,1 px), Enter aplica, Esc cancela. Depois de aplicar a grade fica oculta; "Mostrar grade calibrada" nas propriedades do mapa a exibe. Cada mapa tem a sua calibração e a régua usa a do mapa onde a medição começa.
- Zonas: selecione para ver as alças. Os quadrados brancos nos cantos e lados redimensionam (Shift num canto mantém a proporção). No polígono, os pontos azuis movem vértices, duplo clique numa aresta insere vértice e botão direito num vértice remove. Nome, cor e descrição ficam no popup do botão direito.
- Camadas: novos itens vão para a camada ativa (destacada). "Travar" impede selecionar e mover, e os cliques passam para as camadas de baixo. Ctrl+M move a seleção para a camada ativa.

## Desempenho (técnicas do PureRef e do Atlas VTT)

- Na importação, cada mapa vira uma pirâmide de tiles de 512 px (nível 0 = tamanho original, cada nível com metade do anterior), gravada em cache no disco. Só o menor nível fica sempre na memória.
- O nível desenhado depende do zoom. Só os tiles visíveis são carregados, em threads, com o pedido mais recente primeiro. Enquanto um tile carrega, aparece o nível mais grosseiro já disponível.
- Tiles decodificados ficam num cache LRU de 1 GB compartilhado entre todos os mapas.
- Tiles em PNG, sem perdas: no zoom máximo os pixels são os da imagem original. Cada tile leva 1 px dos vizinhos para não haver emendas.
- Cache em `%LOCALAPPDATA%\VaxMaps\Vax Maps\cache\tiles2`, no teste um mapa texturizado de 6000 x 4000 px ocupou 32 MB. Pode ser apagado com o programa fechado.

## Projeto `.vaxmap`

JSON com camadas e itens. Os mapas são referenciados pelo caminho (relativo ao projeto quando possível), não embutidos: mantenha as imagens junto do projeto ao copiá-lo.

## Compilar

Windows (Qt 6.4+ com MinGW ou MSVC, CMake):

```
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.x/mingw_64
cmake --build build --config Release
```

Linux: `cmake -S . -B build && cmake --build build`.

Testes: `VaxMaps --selftest <pasta> [--bench]` (no Linux sem tela: `QT_QPA_PLATFORM=offscreen`).
O `.exe` distribuído foi compilado a partir do Linux com Qt 6.4.2 estático e MinGW: veja `tools/`.
