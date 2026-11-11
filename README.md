# Vax Maps

Canvas infinito para mapas de RPG: vários mapas grandes na mesma página, desenho livre, textos e notas, camadas, zonas e régua calibrada pela grade do mapa. C++17 + Qt 6 Widgets.

## Uso

| Tecla | Ferramenta |
|---|---|
| V | Selecionar e mover (Del exclui) |
| H, botão do meio, Espaço + arrastar | Mover a vista |
| Roda do mouse | Zoom no cursor |
| P / E | Caneta / Borracha |
| T / N | Texto / Nota (duplo clique edita) |
| M | Régua |
| K | Calibrar mapa (setas ajustam a grade, Shift = 0,1 px) |
| R / O / G | Zona retângulo / círculo / vértices (Shift = quadrado/círculo) |
| S | Snap na grade do mapa calibrado |

- Importar mapas: arraste arquivos para a janela ou Ctrl+I. Formatos: PNG, JPG, WebP, TIFF, BMP, GIF.
- Calibrar: com K, clique numa interseção da grade, depois noutra na mesma linha ou coluna alguns quadrados adiante, e informe quantos quadrados há entre elas e quanto vale um quadrado (ex.: 1,5 m). Cada mapa tem a sua calibração. A régua usa a do mapa onde a medição começa.
- Zonas: nome, cor e descrição no painel Propriedades. Polígono: duplo clique numa aresta insere vértice, botão direito num vértice remove.
- Camadas: novos itens vão para a camada ativa (destacada). "Travar" impede selecionar e mover, e os cliques passam para as camadas de baixo. Ctrl+M move a seleção para a camada ativa.

## Desempenho (técnicas do PureRef e do Atlas VTT)

- Na importação, cada mapa vira uma pirâmide de tiles de 512 px (nível 0 = tamanho original, cada nível com metade do anterior), gravada em cache no disco. Só o menor nível fica sempre na memória.
- O nível desenhado depende do zoom. Só os tiles visíveis são carregados, em threads, com o pedido mais recente primeiro. Enquanto um tile carrega, aparece o nível mais grosseiro já disponível.
- Tiles decodificados ficam num cache LRU de 1 GB compartilhado entre todos os mapas.
- Tiles JPEG para imagens opacas, PNG só quando há transparência. Cada tile leva 1 px dos vizinhos para não haver emendas.
- Cache em `%LOCALAPPDATA%\VaxMaps\Vax Maps\cache\tiles`. Pode ser apagado com o programa fechado.

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
