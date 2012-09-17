/*
 * Copyright (c) 2012, Red Hat.
 * Copyright (c) 2012, Nathan Scott.  All Rights Reserved.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */ 
#include <qwt_plot_directpainter.h>
#include <qwt_plot_marker.h>
#include <qwt_symbol.h>
#include "tracing.h"
#include "main.h"

TracingItem::TracingItem(Chart *parent,
	QmcMetric *mp, pmMetricSpec *msp, pmDesc *dp, const char *legend)
	: ChartItem(mp, msp, dp, legend)
{
    my.spanSymbol = new QwtIntervalSymbol(QwtIntervalSymbol::Box);
    my.spanCurve = new QwtPlotIntervalCurve(label());
    my.spanCurve->setStyle(QwtPlotIntervalCurve::NoCurve);
    my.spanCurve->setOrientation(Qt::Horizontal);
    my.spanCurve->setSymbol(my.spanSymbol);
    my.spanCurve->setZ(1);	// lowest/furthest
    my.spanCurve->attach(parent);

    my.dropSymbol = new QwtIntervalSymbol(QwtIntervalSymbol::Box);
    my.dropCurve = new QwtPlotIntervalCurve(label());
    my.dropCurve->setStyle(QwtPlotIntervalCurve::NoCurve);
    my.dropCurve->setOrientation(Qt::Vertical);
    my.dropCurve->setSymbol(my.dropSymbol);
    my.dropCurve->setZ(2);	// middle/central
    my.dropCurve->attach(parent);

    my.pointSymbol = new QwtSymbol(QwtSymbol::Ellipse);
    my.pointCurve = new QwtPlotCurve(label());
    my.pointCurve->setStyle(QwtPlotCurve::NoCurve);
    my.pointCurve->setSymbol(my.pointSymbol);
    my.pointCurve->setZ(3);	// highest/closest
    my.pointCurve->attach(parent);
}

TracingItem::~TracingItem(void)
{
    delete my.pointSymbol;
    delete my.pointCurve;

    delete my.spanSymbol;
    delete my.spanCurve;

    delete my.dropSymbol;
    delete my.dropCurve;
}

QwtPlotItem* TracingItem::item(void)
{
    return my.pointCurve;
}

QwtPlotCurve* TracingItem::curve(void)
{
    return my.pointCurve;
}

void TracingItem::resetValues(int)
{
    console->post(PmChart::DebugForce, "TracingItem::resetValue: %d events", my.events.size());
}

TraceEvent::TraceEvent(QmcEventRecord const &record)
{
    my.timestamp = tosec(*record.timestamp());
    my.missed = record.missed();
    my.flags = record.flags();
    my.spanID = record.identifier();
    my.rootID = record.parent();
    my.description = record.parameterSummary();
}

void TracingItem::updateValues(bool, bool, pmUnits*, int size, double left, double right, double delta)
{
    console->post(PmChart::DebugForce, "TracingItem::updateValues: "
		"%d events, size: %d left: %.2f right=%.2f delta=%.2f",
		my.events.size(), size, left, right, delta);
#if 0

    QmcMetric *metric = ChartItem::my.metric;

    if (metric->numValues() == 0)
	return;

    // TODO: walk the vectors/lists and drop no-longer-needed events

    //
    // First lookup the event "slot" (aka "span" / y-axis-entry)
    // Strategy is to use the ID from the event (if one exists),
    // which must be mapped to an y-axis zero-indexed value.  If
    // no ID, we fall back to metric instance ID, else just zero.
    //
    int slot = metric->hasInstances() > 0 ? metric->instIndex(0) : 0;
    int parentSlot = 0;

    //
    // Fetch the new set of events, merging them into the existing sets
    // - "Points" curve has an entry for *every* event to be displayed.
    // - "Span" curve has an entry for all *begin/end* flagged events.
    //   These are initially unpaired, unless PMDA is playing games, and
    //   so usually min/max is used as a placeholder until corresponding
    //   begin/end event arrives.
    // - "Drop" curve has an entry to match up events with the parents.
    //   The parent is the root "span" (terminology on loan from Dapper)
    //
    if (metric->error(0) == false) {
	QVector<QmcEventRecord> const &records = metric->eventRecords(0);

	for (int i = 0; i < records.size(); i++) {
	    QmcEventRecord const &record = records.at(i);

	    TraceEvent *event = new TraceEvent(record);
	    double timestamp = event->timestamp();
	    int flags = event->flags();

	    my.events.append(event);

	    if (flags & PM_EVENT_FLAG_ID) {	// lookup ID in yMap
#if 0
		slot = ...;	// VERY IMPORTANT!  override slot before the above
		my.drops.append(QwtIntervalSample(slot,
				QwtInterval(timestamp, 
#endif
	    }

	    // this adds the basic point (ellipse), all events get one
	    my.points.append(QPointF(timestamp, slot));

#if 0
	    if (flags & PM_EVENT_FLAG_PARENT) {	// lookup parent in yMap
		parentSlot = ...;
		// do this on start/end only?  (or if first?)
		my.drops.append(QwtIntervalSample(timestamp,
				    QwtInterval(slot, parentSlot)));
	    }

	    if (flags & PM_EVENT_FLAG_START) {
		if (!my.spans.isEmpty()) {
		    QwtIntervalSample &active = my.spans.last();
		    // did we get a start, then another start?
		    // (if so, just end the previous span now)
		    if (active.interval.maxValue() == DBL_MAX)
			active.interval.setMaxValue(timestamp);
		}
		// no matter what we'll start a new span here
		my.spans.append(QwtIntervalSample(index,
				    QwtInterval(timestamp, DBL_MAX)));
	    }
	    if (flags & PM_EVENT_FLAG_END) {
		if (!my.spans.isEmpty()) {
		    QwtIntervalSample &active = my.spans.last();
		    // did we get an end, then another end?
		    // (if so, move previous span end to now)
		    if (active.interval.maxValue() != DBL_MAX)
			active.interval.setMaxValue(timestamp);
		} else {
		    // got an end, but we haven't seen a start
		    my.spans.append(QwtIntervalSample(index,
				    QwtInterval(DBL_MIN, timestamp)));
		}
	    }
	    // TODO: handle missed events -> event.missed()
	    // Could have a separate list of events? (render differently)
#endif
	}
    } else {
	//
	// Hmm, what does this mean?  Host down, for example.
	// We need some visual representation that makes sense.
	//
    }
#else
    // finally, update the display

    double range = right - left;
    double start, end;

    // test data, proof-of-concept stuff...
    my.points.clear();
    my.spans.clear();
    my.drops.clear();

    start = (left + drand48() * range);
    end = (right - drand48() * range);
    my.spans.append(QwtIntervalSample(0.0, QwtInterval(start, end)));
    my.points.append(QPointF(start, 0.0));
    my.points.append(QPointF(end, 0.0));

    start = (left + drand48() * range);
    end = (right - drand48() * range);
    my.spans.append(QwtIntervalSample(1.0, QwtInterval(start, end)));
    my.points.append(QPointF(start, 1.0));
    my.points.append(QPointF(end, 1.0));

    start = (left + drand48() * range);
    end = (right - drand48() * range);
    my.spans.append(QwtIntervalSample(2.0, QwtInterval(start, end)));
    my.points.append(QPointF(start, 2.0));
    my.points.append(QPointF(end, 2.0));

    my.drops.append(QwtIntervalSample(start, QwtInterval(1.0, 2.0)));
    my.drops.append(QwtIntervalSample(end, QwtInterval(1.0, 2.0)));

    my.points.append(QPointF((left + drand48() * range), 0.0));
    my.points.append(QPointF((left + drand48() * range), 1.0));
    my.points.append(QPointF((left + drand48() * range), 2.0));
    my.points.append(QPointF((right - drand48() * range), 0.0));
    my.points.append(QPointF((right - drand48() * range), 1.0));
    my.points.append(QPointF((right - drand48() * range), -1.0));
    my.points.append(QPointF((right - drand48() * range), 3.0));

#endif

    my.dropCurve->setSamples(my.drops);
    my.spanCurve->setSamples(my.spans);
    my.pointCurve->setSamples(my.points);
}

void TracingItem::rescaleValues(pmUnits*)
{
    console->post(PmChart::DebugForce, "TracingItem::rescaleValues");
}

void TracingItem::setStroke(Chart::Style, QColor color, bool)
{
    QColor alphaColor = color;
    console->post(PmChart::DebugForce, "TracingItem::setStroke");

    QPen outline(QColor(Qt::black));
    alphaColor.setAlpha(196);
    QBrush brush(alphaColor);

    my.spanSymbol->setWidth(6);
    my.spanSymbol->setBrush(brush);
    my.spanSymbol->setPen(outline);

    my.dropSymbol->setWidth(1);
    my.dropSymbol->setBrush(Qt::NoBrush);
    my.dropSymbol->setPen(outline);

    my.pointSymbol->setSize(8);
    my.pointSymbol->setColor(color);
    my.pointSymbol->setPen(outline);
}

void TracingItem::showCursor(bool selected, const QPointF &, int index)
{
    const QBrush brush = my.pointSymbol->brush();

    console->post(PmChart::DebugForce,
		  "TracingItem::showCursor: selected=%c index=%d",
		  selected? 'y' : 'n', index);

    if (selected)
	my.pointSymbol->setBrush(my.pointSymbol->brush().color().dark(180));

    QwtPlotDirectPainter directPainter;
    directPainter.drawSeries(my.pointCurve, index, index);

    if (selected)
	my.pointSymbol->setBrush(brush);   // reset brush
}

void TracingItem::showPoints(const QRectF &rect)
{
    console->post(PmChart::DebugForce, "TracingItem::showPoints: "
		  "top left x,y=%f,%f bottom right x,y=%f,%f",
		  rect.topLeft().x(), rect.topLeft().y(),
		  rect.bottomRight().x(), rect.bottomRight().y());
}

void TracingItem::replot(int history, double*)
{
    console->post(PmChart::DebugForce,
		  "TracingItem::replot: %d event records (%d samples)",
		  my.events.size(), history);

    my.dropCurve->setSamples(my.drops);
    my.spanCurve->setSamples(my.spans);
    my.pointCurve->setSamples(my.points);
}

void TracingItem::revive(Chart *parent)	// TODO: inheritance, move to ChartItem
{
    if (removed()) {
        setRemoved(false);
        my.dropCurve->attach(parent);
        my.spanCurve->attach(parent);
        my.pointCurve->attach(parent);
    }
}

void TracingItem::remove(void)		// TODO: inheritance, move to ChartItem?
{
    setRemoved(true);
    my.dropCurve->detach();
    my.spanCurve->detach();
    my.pointCurve->detach();
}

void TracingItem::setPlotEnd(int)
{
    console->post(PmChart::DebugForce, "TracingItem::setPlotEnd");
}


TracingScaleEngine::TracingScaleEngine()
{
}

void TracingScaleEngine::setScale(bool autoScale, double minValue, double maxValue)
{
    (void)autoScale;
    (void)minValue;
    (void)maxValue;
}

void TracingScaleEngine::autoScale(int maxSteps, double &minValue,
                           double &maxValue, double &stepSize) const
{
    (void)maxSteps;
    (void)minValue;
    (void)maxValue;
    (void)stepSize;
}
