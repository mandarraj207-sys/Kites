#pragma once
#include "ui/circuit_scene.h"

namespace Kites
{
class RVSSCircuitScene : public CircuitScene
{
    Q_OBJECT
    public:
        explicit RVSSCircuitScene(QObject *parent = nullptr)
        :CircuitScene(parent)
        {
            qDebug() << "Loading RVSS Circuit Scene";
            loadScene(":/circuit_designs/SSE.json");
        }
        
};
} // namespace Kites