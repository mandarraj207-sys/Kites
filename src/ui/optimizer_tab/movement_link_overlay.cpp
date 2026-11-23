#include "ui/optimizer_tab/movement_link_overlay.h"

#include "ui/optimizer_tab/instruction_listing.h"

#include <QEvent>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QSplitter>

#include <algorithm>
#include <cmath>

namespace Kites {
namespace {

constexpr qreal kLineWidth = 2.2;
constexpr qreal kGlowWidth = 6.0;
constexpr int kGlowAlpha = 60;
constexpr qreal kDotRadius = 3.5;
constexpr qreal kStartGap = 10.0;  // space between the end of the original text and the curve
constexpr qreal kEndInset = 3.0;   // how far into the optimized listing the curve ends
constexpr qreal kMinReach = 40.0;  // horizontal pull of the curve's control points
constexpr qreal kMaxReach = 180.0;
constexpr qreal kReachFactor = 0.55;
constexpr int kBandAlpha = 40;
constexpr qreal kLabelPadding = 4.0;
constexpr qreal kLabelFontScale = 0.9;

// Same colours the listings use for moved rows.
const QColor kMovedEarlierColor(46, 160, 120);
const QColor kMovedLaterColor(224, 150, 40);

} // namespace

MovementLinkOverlay::MovementLinkOverlay(QSplitter *splitter, InstructionListingTextEdit *original,
                                         InstructionListingTextEdit *optimized)
    : QWidget(splitter->parentWidget()),
      m_splitter(splitter),
      m_original(original),
      m_optimized(optimized)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);

    setAutoFillBackground(false);
    setFocusPolicy(Qt::NoFocus);
    m_splitter->installEventFilter(this);
    syncGeometry();
    show();
}

void MovementLinkOverlay::setMapping(const std::vector<int> &optimizedToOriginal,
                                     std::size_t originalCount)
{
    m_optimizedToOriginal = optimizedToOriginal;
    m_originalToOptimized.assign(originalCount, -1);
    for (std::size_t optimized = 0; optimized < optimizedToOriginal.size(); ++optimized)
    {
        const int from = optimizedToOriginal[optimized];
        if (from >= 0 && static_cast<std::size_t>(from) < originalCount)
            m_originalToOptimized[static_cast<std::size_t>(from)] = static_cast<int>(optimized);
    }
    m_hoverSide.reset();
    update();
}

void MovementLinkOverlay::setHover(Side side, std::optional<std::size_t> instruction)
{
    if (!instruction)
    {
        // Leaving one listing must not wipe a hover that already moved to the other one.
        if (m_hoverSide && *m_hoverSide == side)
            m_hoverSide.reset();
    }
    else
    {
        m_hoverSide = side;
        m_hoverIndex = *instruction;
    }
    update();
}

void MovementLinkOverlay::clear()
{
    m_optimizedToOriginal.clear();
    m_originalToOptimized.clear();
    m_hoverSide.reset();
    update();
}

bool MovementLinkOverlay::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_splitter &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move ||
         event->type() == QEvent::Show))
        syncGeometry();
    return QWidget::eventFilter(watched, event);
}

void MovementLinkOverlay::syncGeometry()
{
    setGeometry(m_splitter->geometry());
    raise();
}

std::optional<MovementLinkOverlay::Link> MovementLinkOverlay::currentLink() const
{
    if (!m_hoverSide)
        return std::nullopt;

    if (*m_hoverSide == Side::Original)
    {
        if (m_hoverIndex >= m_originalToOptimized.size() || m_originalToOptimized[m_hoverIndex] < 0)
            return std::nullopt;
        return Link{m_hoverIndex, static_cast<std::size_t>(m_originalToOptimized[m_hoverIndex])};
    }

    if (m_hoverIndex >= m_optimizedToOriginal.size())
        return std::nullopt;
    const int from = m_optimizedToOriginal[m_hoverIndex];
    if (from < 0 || static_cast<std::size_t>(from) >= m_originalToOptimized.size())
        return std::nullopt;
    return Link{static_cast<std::size_t>(from), m_hoverIndex};
}

void MovementLinkOverlay::paintEvent(QPaintEvent *)
{
    const auto link = currentLink();
    if (!link)
        return;

    const auto from = m_original->instructionGeometry(link->original);
    const auto to = m_optimized->instructionGeometry(link->optimized);
    if (!from || !to)
        return;

    // Viewport (listing) coordinates -> this overlay's coordinates.
    const auto viewRect = [this](const InstructionListingTextEdit *view) {
        QWidget *viewport = view->viewport();
        return QRectF(QPointF(mapFromGlobal(viewport->mapToGlobal(QPoint(0, 0)))),
                      QSizeF(viewport->size()));
    };
    const QRectF originalView = viewRect(m_original);
    const QRectF optimizedView = viewRect(m_optimized);
    const QRectF originalRow = from->row.translated(originalView.topLeft());
    const QRectF optimizedRow = to->row.translated(optimizedView.topLeft());

    QColor color = palette().color(QPalette::Highlight);
    if (link->optimized < link->original)
        color = kMovedEarlierColor;
    else if (link->optimized > link->original)
        color = kMovedLaterColor;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipRect(originalView.united(optimizedView));

    // Tint both rows so the two ends are easy to find.
    QColor band = color;
    band.setAlpha(kBandAlpha);
    painter.fillRect(originalRow.intersected(originalView), band);
    painter.fillRect(optimizedRow.intersected(optimizedView), band);

    // A row scrolled out of view keeps its end of the curve at the nearest edge.
    const auto clampedY = [](const QRectF &row, const QRectF &view, bool &visible) {
        const qreal y = row.center().y();
        visible = y >= view.top() && y <= view.bottom();
        return std::clamp(y, view.top() + kDotRadius, view.bottom() - kDotRadius);
    };
    bool startVisible = true;
    bool endVisible = true;
    const QPointF start(std::min(originalView.left() + from->textEndX + kStartGap,
                                 originalView.right() - kDotRadius),
                        clampedY(originalRow, originalView, startVisible));
    const QPointF end(optimizedView.left() + kEndInset,
                      clampedY(optimizedRow, optimizedView, endVisible));

    const qreal reach = std::clamp(std::abs(end.x() - start.x()) * kReachFactor, kMinReach, kMaxReach);
    QPainterPath curve(start);
    curve.cubicTo(start + QPointF(reach, 0), end - QPointF(reach, 0), end);

    QColor glow = color;
    glow.setAlpha(kGlowAlpha);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(glow, kGlowWidth, Qt::SolidLine, Qt::RoundCap));
    painter.drawPath(curve);
    painter.setPen(QPen(color, kLineWidth, Qt::SolidLine, Qt::RoundCap));
    painter.drawPath(curve);

    const auto drawEnd = [&](const QPointF &point, bool visible) {
        painter.setPen(QPen(color, kLineWidth));
        painter.setBrush(visible ? QBrush(color) : QBrush(palette().color(QPalette::Base)));
        painter.drawEllipse(point, kDotRadius, kDotRadius);
    };
    drawEnd(start, startVisible);
    drawEnd(end, endVisible);

    // "#5 -> #2": the instruction's position before and after, in the listings' own numbering.
    QFont font = painter.font();
    font.setPointSizeF(font.pointSizeF() * kLabelFontScale);
    painter.setFont(font);
    const QString text = QString("#%1 \u2192 #%2").arg(link->original + 1).arg(link->optimized + 1);
    const QFontMetricsF metrics(font);
    const QPointF middle = curve.pointAtPercent(0.5);
    const QRectF label(middle.x() - metrics.horizontalAdvance(text) / 2 - kLabelPadding,
                       middle.y() - metrics.height() / 2 - kLabelPadding / 2,
                       metrics.horizontalAdvance(text) + 2 * kLabelPadding,
                       metrics.height() + kLabelPadding);
    painter.setPen(QPen(color, 1.0));
    painter.setBrush(palette().color(QPalette::Base));
    painter.drawRoundedRect(label, 4, 4);
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(label, Qt::AlignCenter, text);
}

} // namespace Kites
