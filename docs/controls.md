# Controles

Referência dos controles atuais do VoxelSpace SDL (teclado, mouse e gamepad).

## Teclado

| Tecla | Ação |
| --- | --- |
| **W** / **S** | Anda para frente / trás |
| **A** / **D** | Strafe esquerda / direita |
| **H** / **G** | Sobe / desce a câmera (no modo isométrico ajusta só a altura acima do chão) |
| **R** / **F** | Inclina o olhar para cima / baixo (pitch; desligado no modo isométrico) |
| **Left Shift** | Sprint (segura) |
| **Space** | Pula (se estiver no chão) |
| **Ctrl + Space** | Restaura a distância de render padrão |
| **C** | Reseta o pitch (horizonte ao centro) |
| **J** / **K** | Diminui / aumenta o `zstep` (detalhe do raycast) |
| **O** | Liga/desliga a camada de teto (ceiling) |
| **Ctrl + O** | Liga/desliga otimização de distância do raycast |
| **I** | Cicla o integer scale da apresentação |
| **V** | Liga/desliga câmera isométrica |
| **T** | Alterna terreno: raycast voxel ↔ malha poligonal |
| **F10** | Alterna backend: software ↔ OpenGL 3.3 |
| **Alt + Enter** | Tela cheia |
| **Esc** | Se o mouse estiver capturado, libera o cursor; senão, fecha o app |

## Mouse

| Entrada | Ação |
| --- | --- |
| **Movimento** (mouse capturado) | Olha em volta (yaw + pitch). No modo isométrico só gira o yaw (órbita). |
| **Ctrl** (segura) | Mostra o cursor em seta e pausa o look; use o botão esquerdo para picking |
| **Botão esquerdo** (com Ctrl) | Clica num Superman para selecionar; com um selecionado, clica no chão para ele andar até lá |
| **Soltar Ctrl** | Volta ao mouse capturado (look) |
| **Esc** | Com Ctrl/picking ativo, cancela o cursor; com mouse capturado, libera o cursor; senão fecha o app |

Na inicialização o mouse já começa capturado (look livre). Segure **Ctrl** para apontar e clicar nos sprites.

## Gamepad

| Controle | Ação |
| --- | --- |
| **Stick esquerdo** | Move (frente/trás e strafe) |
| **Stick direito** | Rotaciona (yaw) e inclina (pitch) |
| **LT** (gatilho esquerdo) | Acelera o movimento do stick esquerdo |
| **RT** (gatilho direito) | Acelera a rotação/pitch do stick direito |
| **LB** / **RB** | Diminui / aumenta a distância de render |
| **Y** | Restaura a distância de render padrão |
| **Click do stick direito (RSB)** | Reseta o pitch |
| **D-Pad esquerda / direita** | Strafe |
| **A** | Pula (se estiver no chão) |
| **B** | Fecha o app |
| **Touchpad** (quando o SDL expõe) | Look livre (yaw + pitch) |

D-Pad cima/baixo não têm ação ligada no código atual.

## Comportamento automático

- **Gravidade** está sempre ativa: a câmera cai até o chão do heightmap ou o topo dos colliders poligonais.
- Colisão horizontal com caixas dos prédios (`.buildings`) empurra a posição para fora.
- **V** (isométrico): trava pitch e altura relativa ao suporte; mouse só orbita em yaw; **H**/**G** mudam a altura isométrica.

## Notas rápidas

- **T** e **F10** são independentes: dá para combinar terreno poligonal com software ou OpenGL.
- Ajuste de distância de render no teclado hoje é só **Ctrl + Space** (reset); no gamepad use **LB**/**RB** e **Y**.
