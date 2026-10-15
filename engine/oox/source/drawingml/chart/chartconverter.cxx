/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the Collabora Office project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file incorporates work covered by the following license notice:
 *
 *   Licensed to the Apache Software Foundation (ASF) under one or more
 *   contributor license agreements. See the NOTICE file distributed
 *   with this work for additional information regarding copyright
 *   ownership. The ASF licenses this file to you under the Apache
 *   License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.apache.org/licenses/LICENSE-2.0 .
 */

#include <com/sun/star/chart2/XChartDocument.hpp>
#include <com/sun/star/chart2/XCoordinateSystemContainer.hpp>
#include <com/sun/star/chart2/XChartTypeContainer.hpp>
#include <com/sun/star/chart2/XDataSeriesContainer.hpp>
#include <com/sun/star/chart2/XAxis.hpp>
#include <com/sun/star/chart2/XCoordinateSystem.hpp>
#include <com/sun/star/chart2/XChartType.hpp>
#include <com/sun/star/chart2/XDataSeries.hpp>
#include <com/sun/star/chart2/XAnyDescriptionAccess.hpp>
#include <com/sun/star/chart2/XDataPointCustomLabelField.hpp>
#include <com/sun/star/chart2/DataPointCustomLabelFieldType.hpp>
#include <com/sun/star/chart2/data/XDataSequence.hpp>
#include <com/sun/star/chart2/data/XDataSource.hpp>
#include <com/sun/star/chart2/data/XLabeledDataSequence.hpp>
#include <com/sun/star/beans/XPropertySet.hpp>
#include <com/sun/star/beans/XPropertySetInfo.hpp>
#include <com/sun/star/util/DateTime.hpp>
#include <com/sun/star/util/XNumberFormats.hpp>
#include <com/sun/star/lang/Locale.hpp>
#include <comphelper/diagnose_ex.hxx>
#include <comphelper/sequence.hxx>
#include <cmath>
#include <com/sun/star/chart2/data/XDataReceiver.hpp>
#include <com/sun/star/util/XNumberFormatsSupplier.hpp>
#include <drawingml/chart/chartspaceconverter.hxx>
#include <drawingml/chart/chartspacemodel.hxx>
#include <drawingml/chart/stylemodel.hxx>
#include <drawingml/chart/colorsmodel.hxx>
#include <docmodel/uno/UnoChartStyle.hxx>
#include <docmodel/uno/UnoChartColorStyle.hxx>
#include <docmodel/styles/ChartStyle.hxx>
#include <docmodel/styles/ChartColorStyle.hxx>
#include <oox/helper/containerhelper.hxx>
#include <oox/core/xmlfilterbase.hxx>
#include <osl/diagnose.h>

#include <oox/drawingml/chart/chartconverter.hxx>

using ::oox::drawingml::chart::DataSequenceModel;
using ::cpo::uno::Any;
namespace oox::drawingml::chart {

using namespace ::com::sun::star;
using namespace ::com::sun::star::chart2;
using namespace ::com::sun::star::chart2::data;
using namespace ::com::sun::star::drawing;
using namespace ::cpo;
using namespace ::cpo::uno;

using ::oox::core::XmlFilterBase;

std::vector<Reference<XDataSequence>> getWorkbookSequences(const Reference<XChartDocument>& xChart)
{
    std::vector<Reference<XDataSequence>> aSequences;
    auto append = [&aSequences](const Reference<XLabeledDataSequence>& xLabeled)
    {
        if (!xLabeled.is())
            return;
        for (const auto& xSequence : { xLabeled->getLabel(), xLabeled->getValues() })
        {
            if (!xSequence.is())
                continue;
            ENSURE_OR_THROW(aSequences.size() < 16384, "Chart workbook exceeds the sequence limit");
            aSequences.push_back(xSequence);
        }
    };
    Reference<XCoordinateSystemContainer> xSystems(xChart->getFirstDiagram(), UNO_QUERY_THROW);
    for (const auto& xSystem : xSystems->getCoordinateSystems())
    {
        for (sal_Int32 nDimension = 0; nDimension < xSystem->getDimension(); ++nDimension)
            for (sal_Int32 nAxis = 0; nAxis <= xSystem->getMaximumAxisIndexByDimension(nDimension);
                 ++nAxis)
                if (auto xAxis = xSystem->getAxisByDimension(nDimension, nAxis); xAxis.is())
                    append(xAxis->getScaleData().Categories);
        Reference<XChartTypeContainer> xTypes(xSystem, UNO_QUERY_THROW);
        for (const auto& xType : xTypes->getChartTypes())
        {
            Reference<XDataSeriesContainer> xSeriesContainer(xType, UNO_QUERY_THROW);
            for (const auto& xSeries : xSeriesContainer->getDataSeries())
            {
                Reference<XDataSource> xSource(xSeries, UNO_QUERY_THROW);
                for (const auto& xSequence : xSource->getDataSequences())
                    append(xSequence);
                Reference<beans::XPropertySet> xProperties(xSeries, UNO_QUERY_THROW);
                for (const OUString& rName : { u"ErrorBarX"_ustr, u"ErrorBarY"_ustr })
                {
                    Reference<XDataSource> xErrorBars;
                    xProperties->getPropertyValue(rName) >>= xErrorBars;
                    if (xErrorBars.is())
                        for (const auto& xSequence : xErrorBars->getDataSequences())
                            append(xSequence);
                }
            }
        }
    }
    return aSequences;
}

OUString getWorkbookFormula(const Reference<XDataSequence>& xSequence)
{
    Reference<beans::XPropertySet> xProperties(xSequence, UNO_QUERY);
    OUString sFormula;
    if (xProperties.is()
        && xProperties->getPropertySetInfo()->hasPropertyByName(u"ChartExFormula"_ustr))
        xProperties->getPropertyValue(u"ChartExFormula"_ustr) >>= sFormula;
    return sFormula;
}

util::Date getWorkbookDate(const Reference<XChartDocument>& xChart)
{
    Reference<beans::XPropertySet> xProperties(xChart, UNO_QUERY_THROW);
    util::DateTime aDate;
    if (xProperties->getPropertyValue(u"NullDate"_ustr) >>= aDate)
        return util::Date(aDate.Day, aDate.Month, aDate.Year);
    Reference<util::XNumberFormatsSupplier> xSupplier(xChart, UNO_QUERY_THROW);
    util::Date aDefault;
    ENSURE_OR_THROW(xSupplier->getNumberFormatSettings()->getPropertyValue(u"NullDate"_ustr)
                    >>= aDefault,
                    "Chart has no date epoch");
    return aDefault;
}

Sequence<Any> getWorkbookFormat(const Reference<XChartDocument>& xChart,
                                const Reference<XDataSequence>& xSequence)
{
    Reference<util::XNumberFormatsSupplier> xSupplier(xChart, UNO_QUERY_THROW);
    const auto xFormat
        = xSupplier->getNumberFormats()->getByKey(xSequence->getNumberFormatKeyByIndex(-1));
    OUString sCode;
    lang::Locale aLocale;
    xFormat->getPropertyValue(u"FormatString"_ustr) >>= sCode;
    xFormat->getPropertyValue(u"Locale"_ustr) >>= aLocale;
    return { Any(sCode), Any(aLocale.Language), Any(aLocale.Country), Any(aLocale.Variant) };
}

Sequence<Any> getWorkbookSnapshot(const Reference<XChartDocument>& xChart, bool* pHasLabelRanges)
{
    std::vector<Any> aSnapshot;
    sal_Int64 nText = 0;
    sal_Int64 nCells = 0;
    auto append = [&](const Any& rValue)
    {
        ENSURE_OR_THROW(aSnapshot.size() < 2000000, "Chart snapshot exceeds the value limit");
        if (rValue.has<OUString>())
        {
            nText += rValue.get<OUString>().getLength();
            ENSURE_OR_THROW(nText <= 16 * 1024 * 1024, "Chart snapshot exceeds the text limit");
        }
        ENSURE_OR_THROW(!rValue.hasValue() || rValue.has<OUString>() || rValue.has<double>()
                            || rValue.has<sal_Int32>() || rValue.has<bool>(),
                        "Unsupported chart snapshot value type");
        aSnapshot.push_back(rValue.has<double>() && std::isnan(rValue.get<double>()) ? Any()
                                                                                     : rValue);
    };
    auto appendValues = [&](const Sequence<Any>& rValues)
    {
        nCells += rValues.getLength();
        ENSURE_OR_THROW(nCells <= 1000000, "Chart snapshot exceeds the cell limit");
        append(Any(rValues.getLength()));
        for (const auto& rValue : rValues)
            append(rValue);
    };
    const auto aSequences = getWorkbookSequences(xChart);
    append(Any(static_cast<sal_Int32>(aSequences.size())));
    for (const auto& xSequence : aSequences)
    {
        appendValues(xSequence->getData());
        Reference<beans::XPropertySet> xProperties(xSequence, UNO_QUERY_THROW);
        append(xProperties->getPropertyValue(u"Role"_ustr));
        append(Any(getWorkbookFormula(xSequence)));
        for (const auto& rValue : getWorkbookFormat(xChart, xSequence))
            append(rValue);
    }
    const auto aDate = getWorkbookDate(xChart);
    append(Any(sal_Int32(aDate.Year)));
    append(Any(sal_Int32(aDate.Month)));
    append(Any(sal_Int32(aDate.Day)));
    Reference<XAnyDescriptionAccess> xDescriptions(xChart->getDataProvider(), UNO_QUERY_THROW);
    for (const auto& rRows :
         { xDescriptions->getAnyRowDescriptions(), xDescriptions->getAnyColumnDescriptions() })
    {
        append(Any(rRows.getLength()));
        for (const auto& rRow : rRows)
            appendValues(rRow);
    }
    if (pHasLabelRanges)
        *pHasLabelRanges = false;
    sal_Int32 nSeries = 0;
    auto appendLabels = [&](const Reference<beans::XPropertySet>& xProperties, sal_Int32 nPoint)
    {
        Sequence<Reference<XDataPointCustomLabelField>> aFields;
        xProperties->getPropertyValue(u"CustomLabelFields"_ustr) >>= aFields;
        for (sal_Int32 nField = 0; nField < aFields.getLength(); ++nField)
        {
            const auto& xField = aFields[nField];
            if (xField->getFieldType() != DataPointCustomLabelFieldType_CELLRANGE)
                continue;
            append(Any(u"cell-linked-label"_ustr));
            append(Any(nSeries));
            append(Any(nPoint));
            append(Any(nField));
            append(Any(xField->getDataLabelsRange()));
            append(Any(xField->getCellRange()));
            append(Any(xField->getString()));
            if (pHasLabelRanges)
                *pHasLabelRanges = true;
        }
    };
    Reference<XCoordinateSystemContainer> xSystems(xChart->getFirstDiagram(), UNO_QUERY_THROW);
    for (const auto& xSystem : xSystems->getCoordinateSystems())
    {
        Reference<XChartTypeContainer> xTypes(xSystem, UNO_QUERY_THROW);
        for (const auto& xType : xTypes->getChartTypes())
        {
            Reference<XDataSeriesContainer> xSeriesContainer(xType, UNO_QUERY_THROW);
            for (const auto& xSeries : xSeriesContainer->getDataSeries())
            {
                Reference<beans::XPropertySet> xProperties(xSeries, UNO_QUERY_THROW);
                appendLabels(xProperties, -1);
                Sequence<sal_Int32> aPoints;
                xProperties->getPropertyValue(u"AttributedDataPoints"_ustr) >>= aPoints;
                for (sal_Int32 nPoint : aPoints)
                    appendLabels(xSeries->getDataPointByIndex(nPoint), nPoint);
                ++nSeries;
            }
        }
    }
    return comphelper::containerToSequence(aSnapshot);
}

const sal_Unicode API_TOKEN_ARRAY_OPEN      = '{';
const sal_Unicode API_TOKEN_ARRAY_CLOSE     = '}';
const sal_Unicode API_TOKEN_ARRAY_COLSEP    = ';';

static OUString lclGenerateApiArray(const std::vector<Any>& rRow, sal_Int32 nStart, sal_Int32 nCount)
{
    OSL_ENSURE( !rRow.empty(), "ChartConverter::lclGenerateApiArray - missing matrix values" );
    OUStringBuffer aBuffer(( OUStringChar(API_TOKEN_ARRAY_OPEN) ));
    for (auto aBeg = rRow.begin() + nStart, aIt = aBeg, aEnd = aBeg + nCount; aIt != aEnd; ++aIt)
    {
        double fValue = 0.0;
        OUString aString;
        if( aIt != aBeg )
            aBuffer.append( API_TOKEN_ARRAY_COLSEP );
        if( *aIt >>= fValue )
            aBuffer.append( fValue );
        else if( *aIt >>= aString )
        {
            // Code similar to sc/source/filter/oox/formulabase.cxx
            aBuffer.append( "\"" + aString.replaceAll(u"\"", u"\"\"") + "\"" );
        }
        else
            aBuffer.append( "\"\"" );
    }
    aBuffer.append( API_TOKEN_ARRAY_CLOSE );
    return aBuffer.makeStringAndClear();
}

ChartConverter::ChartConverter()
{
}

ChartConverter::~ChartConverter()
{
}

void ChartConverter::convertFromModel( XmlFilterBase& rFilter,
        ChartSpaceModel& rChartModel, const Reference< XChartDocument >& rxChartDoc,
        const Reference< XShapes >& rxExternalPage, const awt::Point& rChartPos,
        const awt::Size& rChartSize ) const
{
    OSL_ENSURE( rxChartDoc.is(), "ChartConverter::convertFromModel - missing chart document" );
    if( rxChartDoc.is() )
    {
        Reference< data::XDataReceiver > xDataReceiver( rxChartDoc, uno::UNO_QUERY_THROW );
        Reference< util::XNumberFormatsSupplier > xNumberFormatsSupplier( rFilter.getModel(), uno::UNO_QUERY );
        if (xNumberFormatsSupplier.is())
            xDataReceiver->attachNumberFormatsSupplier( xNumberFormatsSupplier );

        ConverterRoot aConvBase( rFilter, *this, rChartModel, rxChartDoc, rChartSize );
        ChartSpaceConverter aSpaceConv( aConvBase, rChartModel );
        aSpaceConv.convertFromModel( rxExternalPage, rChartPos );
    }
}

void ChartConverter::createDataProvider( const Reference< XChartDocument >&
        rxChartDoc ) const
{
    try
    {
        if( !rxChartDoc->hasInternalDataProvider() )
            rxChartDoc->createInternalDataProvider( false );
    }
    catch( cpo::uno::Exception& )
    {
    }
}

Reference< XDataSequence > ChartConverter::createDataSequence(
    const Reference< XDataProvider >& rxDataProvider, const DataSequenceModel& rDataSeq,
    const OUString& rRole, const OUString& rRoleQualifier ) const
{
    Reference< XDataSequence > xDataSeq;
    if( rxDataProvider.is() )
    {
        OUString aRangeRep;
        if( !rDataSeq.maData.empty() || (rRole == "values-y" && rDataSeq.mnPointCount > 0) ) try
        {
            // create a single-row array from constant source data
            // (multiple levels in the case of complex categories)
            assert( rDataSeq.mnPointCount > 0);
            std::vector<Any> aRow(rDataSeq.mnLevelCount * rDataSeq.mnPointCount);
            for (auto const& elem : rDataSeq.maData)
                aRow.at(elem.first) = elem.second;

            for (sal_Int32 i = rDataSeq.mnLevelCount-1; i >= 0; i--)
            {
                aRangeRep = lclGenerateApiArray( aRow, i * rDataSeq.mnPointCount, rDataSeq.mnPointCount);

                if (!aRangeRep.isEmpty())
                {
                    // create or add a new level to the data sequence
                    xDataSeq = rxDataProvider->createDataSequenceByValueArray(rRole, aRangeRep, rRoleQualifier);
                    if (i == 0)
                        return xDataSeq;
                }
            }
        }
        catch( cpo::uno::Exception& )
        {
            OSL_FAIL( "ChartConverter::createDataSequence - cannot create data sequence" );
        }
    }

    return nullptr;
}

// ===========
// ChartStyleConverter
// ===========
void ChartStyleConverter::convertFromModel(const XmlFilterBase& rFilter,
        const StyleModel& rChartStyleModel,
        const Reference< XChartStyle >& rxChartStyle)
{
    OSL_ENSURE( rxChartStyle.is(), "ChartStyleConverter::convertFromModel - missing chart style" );
    if (!rxChartStyle.is()) return;

    UnoChartStyle *pUnoCS = static_cast<UnoChartStyle*>(rxChartStyle.get());
    assert(pUnoCS);

    model::StyleSet& aStyles = pUnoCS->getChartStyle();

    if (rChartStyleModel.mxAxisTitle) {
        aStyles.addEntry(model::StyleSet::StyleEntryType::AXISTITLE,
                rChartStyleModel.mxAxisTitle->toStyleEntry(rFilter));
    }
    // etc.
    // TODO: fill in all the stuff

    aStyles.mnId = rChartStyleModel.mnId;
}

// ===========
// ChartColorStyleConverter
// ===========
void ChartColorStyleConverter::convertFromModel([[maybe_unused]] XmlFilterBase& rFilter,
        ColorStyleModel& rChartColorStyleModel,
        const Reference< XChartColorStyle >& rxChartColorStyle)
{
    OSL_ENSURE( rxChartColorStyle.is(), "ChartColorStyleConverter::convertFromModel - missing chart colorstyle" );
    if (!rxChartColorStyle.is()) return;

    UnoChartColorStyle *pUnoCS = static_cast<UnoChartColorStyle*>(rxChartColorStyle.get());
    assert(pUnoCS);

    model::ColorStyleSet& aColorStyles = pUnoCS->getChartColorStyle();

    if (!rChartColorStyleModel.maComplexColors.empty()) {
        aColorStyles.addEntry(rChartColorStyleModel.maComplexColors,
                rChartColorStyleModel.meMethod, rChartColorStyleModel.mnId);
    }
}


} // namespace oox::drawingml::chart

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
