#pragma once

#include <QWidget>

#include <cstddef>
#include <optional>
#include <vector>

class QSplitter;

namespace Kites {

class InstructionListingTextEdit;

/**
 * Transparent layer over the two listings. While the mouse rests on an instruction in either
 * listing it draws a curve to the same instruction in the other one, so it is easy to see where
 * the scheduler moved it. It never takes mouse input.
 */
class MovementLinkOverlay : public QWidget
{
public:
    enum class Side { Original, Optimized };

    MovementLinkOverlay(QSplitter *splitter, InstructionListingTextEdit *original,
                        InstructionListingTextEdit *optimized);

    /** `optimizedToOriginal[i]` is the original position of optimized instruction i (-1 = none). */
    void setMapping(const std::vector<int> &optimizedToOriginal, std::size_t originalCount);

    /** The instruction under the mouse in `side`, or nullopt when the mouse left the rows. */
    void setHover(Side side, std::optional<std::size_t> instruction);

    void clear();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    struct Link
    {
        std::size_t original = 0;
        std::size_t optimized = 0;
    };

    std::optional<Link> currentLink() const;
    void syncGeometry();

    QSplitter *m_splitter = nullptr;
    InstructionListingTextEdit *m_original = nullptr;
    InstructionListingTextEdit *m_optimized = nullptr;

    std::vector<int> m_optimizedToOriginal;
    std::vector<int> m_originalToOptimized;

    std::optional<Side> m_hoverSide;
    std::size_t m_hoverIndex = 0;
};

} // namespace Kites
