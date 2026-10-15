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

#include <XMLFilter.hxx>
#include <MediaDescriptorHelper.hxx>

#include <officecfg/Office/Common.hxx>
#include <svtools/sfxecode.hxx>
#include <comphelper/genericpropertyset.hxx>
#include <comphelper/propertysetinfo.hxx>
#include <comphelper/documentconstants.hxx>
#include <cppuhelper/supportsservice.hxx>
#include <comphelper/sequence.hxx>

#include <osl/diagnose.h>
#include <com/sun/star/beans/NamedValue.hpp>
#include <com/sun/star/beans/PropertyAttribute.hpp>
#include <com/sun/star/beans/XPropertySet.hpp>
#include <com/sun/star/xml/sax/InputSource.hpp>
#include <com/sun/star/xml/sax/Writer.hpp>
#include <com/sun/star/lang/XMultiComponentFactory.hpp>
#include <com/sun/star/lang/XSingleServiceFactory.hpp>
#include <com/sun/star/embed/ElementModes.hpp>
#include <com/sun/star/embed/XStorage.hpp>
#include <com/sun/star/embed/StorageFactory.hpp>
#include <com/sun/star/embed/XTransactedObject.hpp>
#include <com/sun/star/frame/XModel.hpp>
#include <com/sun/star/xml/sax/Parser.hpp>
#include <com/sun/star/xml/sax/SAXParseException.hpp>
#include <com/sun/star/xml/sax/XDocumentHandler.hpp>
#include <com/sun/star/xml/sax/XFastParser.hpp>
#include <com/sun/star/packages/zip/ZipIOException.hpp>
#include <com/sun/star/document/GraphicStorageHandler.hpp>
#include <comphelper/diagnose_ex.hxx>
#include <sal/log.hxx>
#include <tools/stream.hxx>
#include <comphelper/scopeguard.hxx>
#include <comphelper/sequenceashashmap.hxx>
#include <comphelper/propertysequence.hxx>
#include <com/sun/star/beans/XPropertySetInfo.hpp>
#include <com/sun/star/chart2/XChartDocument.hpp>
#include <com/sun/star/chart2/XCoordinateSystemContainer.hpp>
#include <com/sun/star/chart2/XCoordinateSystem.hpp>
#include <com/sun/star/chart2/XChartTypeContainer.hpp>
#include <com/sun/star/chart2/XChartType.hpp>
#include <com/sun/star/chart2/XDataSeriesContainer.hpp>
#include <com/sun/star/chart2/XDataSeries.hpp>
#include <com/sun/star/chart2/XAxis.hpp>
#include <com/sun/star/chart2/data/XDataSource.hpp>
#include <com/sun/star/chart2/data/XDataSequence.hpp>
#include <com/sun/star/chart2/data/XLabeledDataSequence.hpp>
#include <cmath>
#include <cstring>
#include <vector>
#include <map>

using namespace ::com::sun::star;
using namespace ::cpo;

using ::cpo::uno::Reference;
using ::cpo::uno::Sequence;
using ::osl::MutexGuard;

namespace
{
constexpr OUString sXML_metaStreamName = u"meta.xml"_ustr;
constexpr OUString sXML_styleStreamName = u"styles.xml"_ustr;
constexpr OUString sXML_contentStreamName = u"content.xml"_ustr;

// This private storage record has a fixed scalar schema, not a general UNO serializer.
// ZIP stream checksums protect the payload; version, bounds and trailing bytes are checked here.
constexpr OUString sWorkbookState = u"ooxml-chart-workbook-state.bin"_ustr;
constexpr OUString sWorkbookPayload = u"ooxml-chart-workbook.xlsx"_ustr;
constexpr sal_Int32 nWorkbookLimit = 64 * 1024 * 1024;
constexpr sal_uInt32 nWorkbookVersion = 0x57424303;
using uno::Any;
using chart2::XChartDocument;
using chart2::XCoordinateSystemContainer;
using chart2::XChartTypeContainer;
using chart2::XDataSeriesContainer;
using chart2::data::XDataSource;
using chart2::data::XDataSequence;
using chart2::data::XLabeledDataSequence;
using uno::UNO_QUERY_THROW;
struct WorkbookSequence
{
    Reference<XDataSequence> xSequence;
    OUString aOwner;
    OUString aRange;
};

std::vector<WorkbookSequence> workbookSequences(const Reference<XChartDocument>& xChart)
{
    // InternalDataProvider converts these tokens to local-table addresses and back.
    // SchXMLExport also orders the local table by these indices, retaining gaps.
    ENSURE_OR_THROW(xChart->hasInternalDataProvider(), "Unsupported chart workbook provider");
    std::vector<WorkbookSequence> aSequences;
    auto append = [&aSequences](const Reference<XLabeledDataSequence>& xLabeled,
                                const OUString& rOwner, bool bCategories)
    {
        if (!xLabeled.is())
            return;
        sal_Int32 nPart = 0;
        for (const auto& xSequence : { xLabeled->getLabel(), xLabeled->getValues() })
        {
            const bool bLabel = nPart++ == 0;
            if (!xSequence.is())
                continue;
            ENSURE_OR_THROW(aSequences.size() < 16384, "Chart workbook exceeds the sequence limit");
            const OUString aRange = xSequence->getSourceRangeRepresentation();
            if (bCategories)
                ENSURE_OR_THROW(!bLabel && aRange == "categories",
                                "Unsupported chart workbook categories");
            else
            {
                const OUString aIndex
                    = bLabel && aRange.startsWith("label ") ? aRange.copy(6) : aRange;
                const sal_Int32 nIndex = aIndex.toInt32();
                ENSURE_OR_THROW(nIndex >= 0 && nIndex < 16384 && aIndex == OUString::number(nIndex)
                                    && (!bLabel || aRange.startsWith("label ")),
                                "Noncanonical chart workbook range");
            }
            aSequences.push_back(
                { xSequence, rOwner + (bLabel ? u"/label"_ustr : u"/values"_ustr), aRange });
        }
    };
    Reference<XCoordinateSystemContainer> xSystems(xChart->getFirstDiagram(), UNO_QUERY_THROW);
    const auto aSystems = xSystems->getCoordinateSystems();
    ENSURE_OR_THROW(aSystems.getLength() == 1, "Unsupported chart workbook coordinate systems");
    const auto& xSystem = aSystems[0];
    const sal_Int32 nDimensions = xSystem->getDimension();
    ENSURE_OR_THROW(nDimensions == 2 || nDimensions == 3, "Unsupported chart workbook dimensions");
    const OUString aSystem
        = xSystem->getCoordinateSystemType() + "/" + OUString::number(nDimensions);
    for (sal_Int32 nDimension = 0; nDimension < nDimensions; ++nDimension)
    {
        const sal_Int32 nMaximumAxis = xSystem->getMaximumAxisIndexByDimension(nDimension);
        ENSURE_OR_THROW(nMaximumAxis <= 1, "Unsupported chart workbook axes");
        for (sal_Int32 nAxis = 0; nAxis <= nMaximumAxis; ++nAxis)
            if (auto xAxis = xSystem->getAxisByDimension(nDimension, nAxis); xAxis.is())
                append(xAxis->getScaleData().Categories,
                       aSystem + "/axis/" + OUString::number(nDimension) + "/"
                           + OUString::number(nAxis),
                       true);
    }
    Reference<XChartTypeContainer> xTypes(xSystem, UNO_QUERY_THROW);
    // ODF groups imported series by chart type, and can reorder the type containers.
    std::map<OUString, Reference<chart2::XChartType>> aTypes;
    for (const auto& xType : xTypes->getChartTypes())
    {
        const OUString aType = xType->getChartType();
        ENSURE_OR_THROW(aType == "com.sun.star.chart2.ColumnChartType"
                            || aType == "com.sun.star.chart2.LineChartType"
                            || aType == "com.sun.star.chart2.AreaChartType"
                            || aType == "com.sun.star.chart2.PieChartType"
                            || aType == "com.sun.star.chart2.NetChartType"
                            || aType == "com.sun.star.chart2.FilledNetChartType",
                        "Unsupported chart workbook series projection");
        ENSURE_OR_THROW(aTypes.emplace(aType, xType).second, "Ambiguous chart workbook chart type");
    }
    for (const auto& [aType, xType] : aTypes)
    {
        Reference<XDataSeriesContainer> xSeriesContainer(xType, UNO_QUERY_THROW);
        sal_Int32 nSeries = 0;
        for (const auto& xSeries : xSeriesContainer->getDataSeries())
        {
            ENSURE_OR_THROW(nSeries < 16384, "Chart workbook exceeds the series limit");
            Reference<XDataSource> xSource(xSeries, UNO_QUERY_THROW);
            const auto aData = xSource->getDataSequences();
            ENSURE_OR_THROW(aData.getLength() == 1 && aData[0].is() && aData[0]->getValues().is(),
                            "Unsupported chart workbook sequence projection");
            Reference<beans::XPropertySet> xValues(aData[0]->getValues(), UNO_QUERY_THROW);
            OUString aRole;
            ENSURE_OR_THROW((xValues->getPropertyValue(u"Role"_ustr) >>= aRole)
                                && aRole == xType->getRoleOfSequenceForSeriesLabel(),
                            "Unsupported chart workbook main sequence");
            Reference<beans::XPropertySet> xProperties(xSeries, UNO_QUERY_THROW);
            sal_Int32 nAxis = 0;
            ENSURE_OR_THROW((xProperties->getPropertyValue(u"AttachedAxisIndex"_ustr) >>= nAxis)
                                && nAxis >= 0 && nAxis <= 1,
                            "Unsupported chart workbook attached axis");
            for (const OUString& rName : { u"ErrorBarX"_ustr, u"ErrorBarY"_ustr })
            {
                Reference<XDataSource> xErrorBars;
                xProperties->getPropertyValue(rName) >>= xErrorBars;
                ENSURE_OR_THROW(!xErrorBars.is() || !xErrorBars->getDataSequences().hasElements(),
                                "Unsupported chart workbook error-bar sequences");
            }
            // Owner ordinals alone are insufficient: every occurrence also carries its
            // persisted range and exact typed data, checked before formulas are restored.
            append(aData[0],
                   aSystem + "/" + aType + "/" + OUString::number(nSeries++) + "/axis/"
                       + OUString::number(nAxis),
                   false);
            if (aData[0]->getLabel().is())
                ENSURE_OR_THROW(aData[0]->getLabel()->getSourceRangeRepresentation()
                                    == "label "
                                           + aData[0]->getValues()->getSourceRangeRepresentation(),
                                "Unsupported chart workbook label binding");
        }
    }
    return aSequences;
}

void writeWorkbookValues(SvStream& rStream, const Sequence<Any>& rValues);

void workbookCurrentIdentity(const Reference<XChartDocument>& xChart, Sequence<Any>& rData,
                             Sequence<Any>& rFormulas)
{
    std::vector<Any> aData;
    std::vector<Any> aFormulas;
    sal_Int64 nText = 0;
    sal_Int64 nCells = 0;
    sal_Int64 nIdentityBytes = 0;
    std::map<OUString, std::vector<sal_Int8>> aRangeIdentities;
    std::map<OUString, OUString> aRangeFormulas;
    for (const auto& rSequence : workbookSequences(xChart))
    {
        const auto& xSequence = rSequence.xSequence;
        nText += sal_Int64(rSequence.aOwner.getLength()) + rSequence.aRange.getLength();
        aData.emplace_back(rSequence.aOwner);
        aData.emplace_back(rSequence.aRange);
        const size_t nIdentityStart = aData.size();
        Reference<beans::XPropertySet> xProperties(xSequence, UNO_QUERY_THROW);
        const Any aRole = xProperties->getPropertyValue(u"Role"_ustr);
        ENSURE_OR_THROW(aRole.has<OUString>(), "Invalid chart sequence role");
        nText += aRole.get<OUString>().getLength();
        ENSURE_OR_THROW(nText <= 16 * 1024 * 1024, "Chart workbook roles exceed the text limit");
        aData.push_back(aRole);
        const auto aValues = xSequence->getData();
        nCells += aValues.getLength();
        ENSURE_OR_THROW(nCells <= 1000000, "Chart workbook identity exceeds the cell limit");
        aData.emplace_back(aValues.getLength());
        for (const auto& rValue : aValues)
        {
            if (rValue.has<OUString>())
                nText += rValue.get<OUString>().getLength();
            ENSURE_OR_THROW(nText <= 16 * 1024 * 1024,
                            "Chart workbook identity exceeds the text limit");
            aData.push_back(rValue.has<double>() && std::isnan(rValue.get<double>()) ? Any()
                                                                                     : rValue);
        }
        {
            // Equal data at different persisted ranges is unambiguous. A reused range
            // must describe the same typed data, including after ODF splits an alias.
            sal_Int64 nEstimatedBytes = 4;
            for (size_t n = nIdentityStart; n < aData.size(); ++n)
                nEstimatedBytes += aData[n].has<OUString>()
                                       ? 5 + sal_Int64(aData[n].get<OUString>().getLength()) * 2
                                       : 9;
            ENSURE_OR_THROW(nIdentityBytes + nEstimatedBytes <= nWorkbookLimit,
                            "Chart sequence identities exceed the byte limit");
            SvMemoryStream aIdentity;
            aIdentity.SetEndian(SvStreamEndian::LITTLE);
            writeWorkbookValues(
                aIdentity, Sequence<Any>(aData.data() + nIdentityStart,
                                         static_cast<sal_Int32>(aData.size() - nIdentityStart)));
            const auto nBytes = aIdentity.TellEnd();
            nIdentityBytes += nBytes;
            ENSURE_OR_THROW(nIdentityBytes <= nWorkbookLimit,
                            "Chart sequence identities exceed the byte limit");
            const auto* pBytes = static_cast<const sal_Int8*>(aIdentity.GetData());
            std::vector<sal_Int8> aBytes(pBytes, pBytes + nBytes);
            const auto [aIdentityIt, bNewRange]
                = aRangeIdentities.emplace(rSequence.aRange, aBytes);
            ENSURE_OR_THROW(bNewRange || aIdentityIt->second == aBytes,
                            "Conflicting chart workbook range identity");
        }
        OUString sFormula;
        if (xProperties->getPropertySetInfo()->hasPropertyByName(u"ChartExFormula"_ustr))
            xProperties->getPropertyValue(u"ChartExFormula"_ustr) >>= sFormula;
        nText += sFormula.getLength();
        ENSURE_OR_THROW(nText <= 16 * 1024 * 1024, "Chart workbook formulas exceed the text limit");
        const auto [aFormulaIt, bNewFormula] = aRangeFormulas.emplace(rSequence.aRange, sFormula);
        ENSURE_OR_THROW(bNewFormula || aFormulaIt->second == sFormula,
                        "Conflicting chart workbook range formulas");
        aFormulas.emplace_back(sFormula);
    }
    rData = comphelper::containerToSequence(aData);
    rFormulas = comphelper::containerToSequence(aFormulas);
}

void writeWorkbookValues(SvStream& rStream, const Sequence<Any>& rValues)
{
    ENSURE_OR_THROW(rValues.getLength() <= 2000000, "Chart workbook metadata has too many values");
    ENSURE_OR_THROW(rStream.Tell() + 4 <= nWorkbookLimit, "Chart metadata exceeds the byte limit");
    rStream.WriteUInt32(rValues.getLength());
    sal_Int64 nText = 0;
    for (const auto& rValue : rValues)
    {
        if (rValue.has<OUString>())
            nText += rValue.get<OUString>().getLength();
        ENSURE_OR_THROW(nText <= 16 * 1024 * 1024, "Chart metadata exceeds the text limit");
        const sal_Int64 nSize
            = rValue.has<OUString>() ? 5 + sal_Int64(rValue.get<OUString>().getLength()) * 2 : 9;
        ENSURE_OR_THROW(rStream.Tell() + nSize <= nWorkbookLimit,
                        "Chart workbook metadata exceeds the byte limit");
        if (!rValue.hasValue())
            rStream.WriteUInt8(0);
        else if (rValue.has<bool>())
            rStream.WriteUInt8(1).WriteUInt8(rValue.get<bool>());
        else if (rValue.has<sal_Int32>())
            rStream.WriteUInt8(2).WriteInt32(rValue.get<sal_Int32>());
        else if (rValue.has<double>())
            rStream.WriteUInt8(3).WriteDouble(rValue.get<double>());
        else if (rValue.has<OUString>())
        {
            rStream.WriteUInt8(4);
            write_uInt32_lenPrefixed_uInt16s_FromOUString(rStream, rValue.get<OUString>());
        }
        else
            ENSURE_OR_THROW(false, "Unsupported chart workbook metadata type");
    }
    ENSURE_OR_THROW(rStream.good(), "Writing chart workbook metadata failed");
}

Sequence<Any> readWorkbookValues(SvStream& rStream)
{
    sal_uInt32 nCount = 0;
    rStream.ReadUInt32(nCount);
    ENSURE_OR_THROW(nCount <= 2000000 && nCount <= rStream.remainingSize(),
                    "Invalid chart workbook metadata count");
    Sequence<Any> aValues(nCount);
    sal_Int64 nText = 0;
    for (auto& rValue : asNonConstRange(aValues))
    {
        sal_uInt8 nType = 0;
        rStream.ReadUChar(nType);
        switch (nType)
        {
            case 0:
                break;
            case 1:
            {
                sal_uInt8 nValue = 0;
                rStream.ReadUChar(nValue);
                ENSURE_OR_THROW(nValue <= 1, "Invalid chart workbook boolean");
                rValue <<= bool(nValue);
                break;
            }
            case 2:
            {
                sal_Int32 nValue = 0;
                rStream.ReadInt32(nValue);
                rValue <<= nValue;
                break;
            }
            case 3:
            {
                double fValue = 0;
                rStream.ReadDouble(fValue);
                rValue <<= fValue;
                break;
            }
            case 4:
            {
                sal_uInt32 nLength = 0;
                rStream.ReadUInt32(nLength);
                nText += nLength;
                ENSURE_OR_THROW(nText <= 16 * 1024 * 1024
                                    && sal_uInt64(nLength) * 2 <= rStream.remainingSize(),
                                "Invalid chart workbook text length");
                rValue <<= read_uInt16s_ToOUString(rStream, nLength);
                break;
            }
            default:
                ENSURE_OR_THROW(false, "Unknown chart workbook metadata type");
        }
        ENSURE_OR_THROW(rStream.good(), "Truncated chart workbook metadata");
    }
    return aValues;
}

Sequence<sal_Int8> readWorkbookStream(const Reference<embed::XStorage>& xStorage,
                                      const OUString& rName)
{
    auto xStream = xStorage->openStreamElement(rName, embed::ElementModes::READ);
    auto xInput = xStream->getInputStream();
    comphelper::ScopeGuard aClose([&] { xInput->closeInput(); });
    Sequence<sal_Int8> aBytes;
    const sal_Int32 nBytes = xInput->readBytes(aBytes, nWorkbookLimit + 1);
    ENSURE_OR_THROW(nBytes > 0 && nBytes <= nWorkbookLimit, "Invalid chart workbook stream size");
    return aBytes;
}

void writeWorkbookStream(const Reference<embed::XStorage>& xStorage, const OUString& rName,
                         const Sequence<sal_Int8>& rBytes, const OUString& rMediaType)
{
    ENSURE_OR_THROW(rBytes.getLength() <= nWorkbookLimit,
                    "Chart workbook stream exceeds the byte limit");
    auto xStream = xStorage->openStreamElement(rName, embed::ElementModes::READWRITE
                                                          | embed::ElementModes::TRUNCATE);
    Reference<beans::XPropertySet> xProperties(xStream, UNO_QUERY_THROW);
    xProperties->setPropertyValue(u"MediaType"_ustr, Any(rMediaType));
    xProperties->setPropertyValue(u"UseCommonStoragePasswordEncryption"_ustr, Any(true));
    auto xOutput = xStream->getOutputStream();
    comphelper::ScopeGuard aClose([&] { xOutput->closeOutput(); });
    xOutput->writeBytes(rBytes);
}

void storeWorkbook(const Reference<lang::XComponent>& xDocument,
                   const Reference<embed::XStorage>& xStorage)
{
    Reference<XChartDocument> xChart(xDocument, UNO_QUERY_THROW);
    Reference<beans::XPropertySet> xDiagram(xChart->getFirstDiagram(), UNO_QUERY_THROW);
    const Any aProperty = xDiagram->getPropertyValue(u"OOXMLWorkbook"_ustr);
    if (!aProperty.hasValue())
    {
        for (const auto& rName : { sWorkbookState, sWorkbookPayload })
            if (xStorage->hasByName(rName))
                xStorage->removeElement(rName);
        return;
    }
    comphelper::SequenceAsHashMap aRecord(aProperty);
    const bool bUnavailable = aRecord.find(u"Unavailable"_ustr) != aRecord.end();
    SvMemoryStream aStream;
    aStream.SetEndian(SvStreamEndian::LITTLE);
    aStream.WriteUInt32(nWorkbookVersion).WriteUInt8(bUnavailable);
    if (!bUnavailable)
    {
        Sequence<Any> aOriginal;
        Sequence<sal_Int8> aBytes;
        ENSURE_OR_THROW((aRecord[u"Data"_ustr] >>= aOriginal) && (aRecord[u"Bytes"_ustr] >>= aBytes)
                            && aBytes.hasElements(),
                        "Incomplete original chart workbook record");
        Sequence<Any> aCurrent;
        Sequence<Any> aFormulas;
        workbookCurrentIdentity(xChart, aCurrent, aFormulas);
        writeWorkbookValues(aStream, aOriginal);
        writeWorkbookValues(aStream, aCurrent);
        writeWorkbookValues(aStream, aFormulas);
        writeWorkbookStream(
            xStorage, sWorkbookPayload, aBytes,
            u"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"_ustr);
    }
    else if (xStorage->hasByName(sWorkbookPayload))
        xStorage->removeElement(sWorkbookPayload);
    writeWorkbookStream(xStorage, sWorkbookState,
                        Sequence<sal_Int8>(static_cast<const sal_Int8*>(aStream.GetData()),
                                           static_cast<sal_Int32>(aStream.TellEnd())),
                        u"application/octet-stream"_ustr);
}

void loadWorkbook(const Reference<lang::XComponent>& xDocument,
                  const Reference<embed::XStorage>& xStorage)
{
    if (!xStorage->hasByName(sWorkbookState) && !xStorage->hasByName(sWorkbookPayload))
        return;
    Reference<XChartDocument> xChart(xDocument, UNO_QUERY_THROW);
    Reference<beans::XPropertySet> xDiagram(xChart->getFirstDiagram(), UNO_QUERY_THROW);
    try
    {
        auto aMetadata = readWorkbookStream(xStorage, sWorkbookState);
        SvMemoryStream aStream(aMetadata.getArray(), aMetadata.getLength(), StreamMode::READ);
        aStream.SetEndian(SvStreamEndian::LITTLE);
        sal_uInt32 nVersion = 0;
        sal_uInt8 nUnavailable = 0;
        aStream.ReadUInt32(nVersion).ReadUChar(nUnavailable);
        ENSURE_OR_THROW(nVersion == nWorkbookVersion && nUnavailable == 0,
                        "Unsupported or unavailable chart workbook record");
        const auto aOriginal = readWorkbookValues(aStream);
        const auto aStoredCurrent = readWorkbookValues(aStream);
        const auto aFormulas = readWorkbookValues(aStream);
        ENSURE_OR_THROW(aStream.good() && aStream.remainingSize() == 0,
                        "Trailing chart workbook metadata");
        Sequence<Any> aCurrent;
        Sequence<Any> aUnusedFormulas;
        workbookCurrentIdentity(xChart, aCurrent, aUnusedFormulas);
        const auto aSequences = workbookSequences(xChart);
        // Use the same typed byte encoding as the range check. UNO Any equality
        // can equate different numeric types and signed zero, which would hide a permutation.
        SvMemoryStream aCurrentBytes;
        SvMemoryStream aStoredBytes;
        aCurrentBytes.SetEndian(SvStreamEndian::LITTLE);
        aStoredBytes.SetEndian(SvStreamEndian::LITTLE);
        writeWorkbookValues(aCurrentBytes, aCurrent);
        writeWorkbookValues(aStoredBytes, aStoredCurrent);
        ENSURE_OR_THROW(aCurrentBytes.TellEnd() == aStoredBytes.TellEnd()
                            && std::memcmp(aCurrentBytes.GetData(), aStoredBytes.GetData(),
                                           static_cast<size_t>(aCurrentBytes.TellEnd()))
                                   == 0
                            && aSequences.size() == static_cast<size_t>(aFormulas.getLength()),
                        "Chart workbook sequence identity changed during native reload");
        std::map<const XDataSequence*, OUString> aObjectFormulas;
        std::map<OUString, OUString> aRangeFormulas;
        for (size_t n = 0; n < aSequences.size(); ++n)
        {
            OUString aFormula;
            ENSURE_OR_THROW(aFormulas[n] >>= aFormula, "Invalid saved chart formula");
            const auto [aObject, bNewObject]
                = aObjectFormulas.emplace(aSequences[n].xSequence.get(), aFormula);
            const auto [aRange, bNewRange] = aRangeFormulas.emplace(aSequences[n].aRange, aFormula);
            ENSURE_OR_THROW((bNewObject || aObject->second == aFormula)
                                && (bNewRange || aRange->second == aFormula),
                            "Conflicting restored chart workbook formulas");
        }
        for (size_t n = 0; n < aSequences.size(); ++n)
        {
            OUString sFormula;
            ENSURE_OR_THROW(aFormulas[n] >>= sFormula, "Invalid saved chart formula");
            Reference<beans::XPropertySet> xProperties(aSequences[n].xSequence, UNO_QUERY_THROW);
            if (xProperties->getPropertySetInfo()->hasPropertyByName(u"ChartExFormula"_ustr))
                xProperties->setPropertyValue(u"ChartExFormula"_ustr, Any(sFormula));
            else
                ENSURE_OR_THROW(sFormula.isEmpty(),
                                "Reloaded chart cannot retain formula metadata");
        }
        xDiagram->setPropertyValue(
            u"OOXMLWorkbook"_ustr,
            Any(comphelper::InitPropertySequence({
                { u"Bytes"_ustr, Any(readWorkbookStream(xStorage, sWorkbookPayload)) },
                { u"Data"_ustr, Any(aOriginal) },
            })));
    }
    catch (const uno::Exception&)
    {
        TOOLS_WARN_EXCEPTION("chart2", "Restoring the original chart workbook failed");
        xDiagram->setPropertyValue(
            u"OOXMLWorkbook"_ustr,
            Any(comphelper::InitPropertySequence({ { u"Unavailable"_ustr, Any(true) } })));
    }
}

uno::Reference< embed::XStorage > lcl_getWriteStorage(
    const Sequence< beans::PropertyValue >& rMediaDescriptor,
    const uno::Reference< cpo::uno::XComponentContext >& xContext,const OUString& _sMediaType)
{
    uno::Reference< embed::XStorage > xStorage;
    try
    {
        apphelper::MediaDescriptorHelper aMDHelper( rMediaDescriptor );
        if( aMDHelper.ISSET_Storage )
        {
            xStorage = aMDHelper.Storage;
        }
        else
        {
            Reference< lang::XSingleServiceFactory > xStorageFact( embed::StorageFactory::create( xContext ) );

            std::vector< beans::PropertyValue > aPropertiesForStorage;

            for( sal_Int32 i=rMediaDescriptor.getLength(); i--; )
            {
                // properties understood by storage factory
                // (see package/source/xstor/xfactory.cxx for details)
                if ( rMediaDescriptor[i].Name == "InteractionHandler" || rMediaDescriptor[i].Name == "Password" || rMediaDescriptor[i].Name == "RepairPackage" )
                {
                    aPropertiesForStorage.push_back( rMediaDescriptor[i] );
                }
            }

            Sequence< cpo::uno::Any > aStorageArgs{
                aMDHelper.ISSET_OutputStream ? cpo::uno::Any(aMDHelper.OutputStream)
                                             : cpo::uno::Any(aMDHelper.URL),
                cpo::uno::Any(embed::ElementModes::READWRITE | embed::ElementModes::TRUNCATE),
                cpo::uno::Any(comphelper::containerToSequence( aPropertiesForStorage ))
            };

            xStorage.set(
                xStorageFact->createInstanceWithArguments( aStorageArgs ),
                uno::UNO_QUERY_THROW );
        }

        // set correct media type at storage
        uno::Reference<beans::XPropertySet> xProp(xStorage,uno::UNO_QUERY);
        OUString aMediaType;
        if ( ! xProp.is() ||
             ! ( xProp->getPropertyValue( u"MediaType"_ustr) >>= aMediaType ) ||
             ( aMediaType.isEmpty() ))
        {
            xProp->setPropertyValue( u"MediaType"_ustr, cpo::uno::Any( _sMediaType ));
        }
    }
    catch (const cpo::uno::Exception&)
    {
        DBG_UNHANDLED_EXCEPTION("chart2");
    }
    return xStorage;
}

uno::Reference< embed::XStorage > lcl_getReadStorage(
    const Sequence< beans::PropertyValue >& rMediaDescriptor,
    const uno::Reference< cpo::uno::XComponentContext >& xContext)
{
    uno::Reference< embed::XStorage > xStorage;

    try
    {
        apphelper::MediaDescriptorHelper aMDHelper( rMediaDescriptor );
        if( aMDHelper.ISSET_Storage )
        {
            xStorage = aMDHelper.Storage;
        }
        else
        {
            // get XStream from MediaDescriptor
            uno::Reference< io::XInputStream > xStream;
            std::vector< beans::PropertyValue > aPropertiesForStorage;
            for( sal_Int32 i=rMediaDescriptor.getLength(); i--; )
            {
                if( rMediaDescriptor[i].Name == "InputStream" )
                    xStream.set( rMediaDescriptor[i].Value, uno::UNO_QUERY );

                // properties understood by storage factory
                // (see package/source/xstor/xfactory.cxx for details)
                if ( rMediaDescriptor[i].Name == "InteractionHandler" || rMediaDescriptor[i].Name == "Password" || rMediaDescriptor[i].Name == "RepairPackage" )
                {
                    aPropertiesForStorage.push_back( rMediaDescriptor[i] );
                }
            }
            OSL_ENSURE( xStream.is(), "No Stream" );
            if( ! xStream.is())
                return xStorage;

            // convert XInputStream to XStorage via the storage factory
            Reference< lang::XSingleServiceFactory > xStorageFact( embed::StorageFactory::create( xContext ) );
            Sequence< cpo::uno::Any > aStorageArgs{
                cpo::uno::Any(xStream),
                cpo::uno::Any(embed::ElementModes::READ | embed::ElementModes::NOCREATE),
                cpo::uno::Any(comphelper::containerToSequence( aPropertiesForStorage ))
            };
            xStorage.set(
                xStorageFact->createInstanceWithArguments( aStorageArgs ), uno::UNO_QUERY_THROW );
        }

        OSL_ENSURE( xStorage.is(), "No Storage" );
    }
    catch (const cpo::uno::Exception&)
    {
        DBG_UNHANDLED_EXCEPTION("chart2");
    }

    return xStorage;
}

} // anonymous namespace

namespace chart
{

XMLFilter::XMLFilter( Reference< cpo::uno::XComponentContext > const & xContext ) :
        m_xContext( xContext )
{}

XMLFilter::~XMLFilter()
{}

// ____ XFilter ____
bool XMLFilter::filter(
    const Sequence< beans::PropertyValue >& aDescriptor )
{
    bool bResult = false;

    MutexGuard aGuard( m_aMutex );

    if( m_xSourceDoc.is())
    {
        OSL_ENSURE( ! m_xTargetDoc.is(), "source doc is set -> target document should not be set" );
        if( impl_Export( m_xSourceDoc,
                         aDescriptor ) == ERRCODE_NONE )
        {
            m_xSourceDoc = nullptr;
            bResult = true;
        }
    }
    else if( m_xTargetDoc.is())
    {
        if( impl_Import( m_xTargetDoc,
                         aDescriptor ) == ERRCODE_NONE )
        {
            m_xTargetDoc = nullptr;
            bResult = true;
        }
    }
    else
    {
        OSL_FAIL( "filter() called with no document set" );
    }

    return bResult;
}

void XMLFilter::cancel()
{
}

// ____ XImporter ____
void XMLFilter::setTargetDocument(
    const Reference< lang::XComponent >& Document )
{
    MutexGuard aGuard( m_aMutex );
    OSL_ENSURE( ! m_xSourceDoc.is(), "Setting target doc while source doc is set" );

    m_xTargetDoc = Document;
}

// ____ XExporter ____
void XMLFilter::setSourceDocument(
    const Reference< lang::XComponent >& Document )
{
    MutexGuard aGuard( m_aMutex );
    OSL_ENSURE( ! m_xTargetDoc.is(), "Setting source doc while target doc is set" );

    m_xSourceDoc = Document;
}

ErrCode XMLFilter::impl_Import(
    const Reference< lang::XComponent > & xDocumentComp,
    const Sequence< beans::PropertyValue > & rMediaDescriptor )
{
    ErrCode nWarning = ERRCODE_NONE;

    OSL_ENSURE( xDocumentComp.is(), "Import: No Model" );
    OSL_ENSURE( m_xContext.is(), "Import: No ComponentContext" );

    if( ! (xDocumentComp.is() &&
           m_xContext.is()))
        return nWarning;

    try
    {
        Reference< lang::XServiceInfo > xServInfo( xDocumentComp, uno::UNO_QUERY_THROW );
        if( ! xServInfo->supportsService( u"com.sun.star.chart2.ChartDocument"_ustr))
        {
            OSL_FAIL( "Import: No ChartDocument" );
            return ERRCODE_SFX_GENERAL;
        }

        Reference< lang::XMultiComponentFactory > xFactory( m_xContext->getServiceManager());
        OSL_ENSURE( xFactory.is(), "Import: No Factory" );
        if( ! xFactory.is())
            return ERRCODE_SFX_GENERAL;

        bool bOasis = true;
        isOasisFormat( rMediaDescriptor, bOasis );
        Reference< embed::XStorage > xStorage( lcl_getReadStorage( rMediaDescriptor, m_xContext));
        if( ! xStorage.is())
            return ERRCODE_SFX_GENERAL;

        uno::Reference<document::XGraphicStorageHandler> xGraphicStorageHandler;
        uno::Reference<lang::XMultiServiceFactory> xServiceFactory(xFactory, uno::UNO_QUERY);
        if (xServiceFactory.is())
        {
            cpo::uno::Sequence<cpo::uno::Any> aArgs{ cpo::uno::Any(xStorage) };
            xGraphicStorageHandler.set(
                xServiceFactory->createInstanceWithArguments(
                    u"com.sun.star.comp.Svx.GraphicImportHelper"_ustr, aArgs), uno::UNO_QUERY);
        }

        // create XPropertySet with extra information for the filter
        /** property map for import info set */
        static comphelper::PropertyMapEntry const aImportInfoMap[] =
        {
            // necessary properties for XML progress bar at load time
            { u"ProgressRange"_ustr,   0, cppu::UnoType<sal_Int32>::get(),  css::beans::PropertyAttribute::MAYBEVOID, 0},
            { u"ProgressMax"_ustr,     0, cppu::UnoType<sal_Int32>::get(),  css::beans::PropertyAttribute::MAYBEVOID, 0},
            { u"ProgressCurrent"_ustr, 0, cppu::UnoType<sal_Int32>::get(),  css::beans::PropertyAttribute::MAYBEVOID, 0},
            { u"PrivateData"_ustr,     0, cppu::UnoType<XInterface>::get(), css::beans::PropertyAttribute::MAYBEVOID, 0 },
            { u"BaseURI"_ustr,         0, cppu::UnoType<OUString>::get(),   css::beans::PropertyAttribute::MAYBEVOID, 0 },
            { u"StreamRelPath"_ustr,   0, cppu::UnoType<OUString>::get(),   css::beans::PropertyAttribute::MAYBEVOID, 0 },
            { u"StreamName"_ustr,      0, cppu::UnoType<OUString>::get(),   css::beans::PropertyAttribute::MAYBEVOID, 0 },
            { u"BuildId"_ustr,         0, cppu::UnoType<OUString>::get(),   css::beans::PropertyAttribute::MAYBEVOID, 0 },
        };
        uno::Reference< beans::XPropertySet > xImportInfo(
                    comphelper::GenericPropertySet_CreateInstance(
                                new comphelper::PropertySetInfo( aImportInfoMap ) ) );

        // Set base URI and Hierarchical Name
        OUString aHierarchName, aBaseUri;
        // why retrieve this from the model when it's available as rMediaDescriptor?
        uno::Reference<frame::XModel> const xModel(m_xTargetDoc, uno::UNO_QUERY);
        if( xModel.is() )
        {
            const cpo::uno::Sequence< beans::PropertyValue > aModProps = xModel->getArgs();
            for( beans::PropertyValue const & prop : aModProps )
            {
                if( prop.Name == "HierarchicalDocumentName" )
                {
                    // Actually this argument only has meaning for embedded documents
                    prop.Value >>= aHierarchName;
                }
                else if( prop.Name == "DocumentBaseURL" )
                {
                    prop.Value >>= aBaseUri;
                }
            }
        }

        // needed for relative URLs, but in clipboard copy/paste there may be none
        SAL_INFO_IF(aBaseUri.isEmpty(), "chart2", "chart::XMLFilter: no base URL");
        if( !aBaseUri.isEmpty() )
            xImportInfo->setPropertyValue( u"BaseURI"_ustr, cpo::uno::Any( aBaseUri ) );

        if( !aHierarchName.isEmpty() )
            xImportInfo->setPropertyValue( u"StreamRelPath"_ustr, cpo::uno::Any( aHierarchName ) );

        // import meta information
        if( bOasis )
            nWarning = impl_ImportStream(
                sXML_metaStreamName,
                u"com.sun.star.comp.Chart.XMLOasisMetaImporter"_ustr,
                xStorage, xFactory, xGraphicStorageHandler, xImportInfo );

        // import styles
        ErrCode nTmpErr = impl_ImportStream(
            sXML_styleStreamName,
            bOasis
            ? u"com.sun.star.comp.Chart.XMLOasisStylesImporter"_ustr
            : u"com.sun.star.comp.Chart.XMLStylesImporter"_ustr,
            xStorage, xFactory, xGraphicStorageHandler, xImportInfo );
        nWarning = nWarning != ERRCODE_NONE ? nWarning : nTmpErr;

        // import content
        ErrCode nContentWarning = impl_ImportStream(
            sXML_contentStreamName,
            bOasis
            ? u"com.sun.star.comp.Chart.XMLOasisContentImporter"_ustr
            : u"com.sun.star.comp.Chart.XMLContentImporter"_ustr,
            xStorage, xFactory, xGraphicStorageHandler, xImportInfo );
        nWarning = nWarning != ERRCODE_NONE ? nWarning : nContentWarning;
        if (nContentWarning == ERRCODE_NONE)
            loadWorkbook(xDocumentComp, xStorage);
    }
    catch (const cpo::uno::Exception&)
    {
        DBG_UNHANDLED_EXCEPTION("chart2");

        // something went awry
        nWarning = ERRCODE_SFX_GENERAL;
    }

    return nWarning;
}

ErrCode XMLFilter::impl_ImportStream(
    const OUString & rStreamName,
    const OUString & rServiceName,
    const Reference< embed::XStorage > & xStorage,
    const Reference< lang::XMultiComponentFactory > & xFactory,
    const Reference< document::XGraphicStorageHandler > & xGraphicStorageHandler,
    uno::Reference< beans::XPropertySet > const & xImportInfo )
{
    ErrCode nWarning = ERRCODE_SFX_GENERAL;

    if( ! (xStorage.is() &&
           xStorage->hasByName( rStreamName )))
        return ERRCODE_NONE;

    if( xImportInfo.is() )
        xImportInfo->setPropertyValue( u"StreamName"_ustr, cpo::uno::Any( rStreamName ) );

    if( xStorage.is() &&
        xStorage->isStreamElement( rStreamName ) )
    {
        try
        {
            auto xInputStream =
                xStorage->openStreamElement(
                    rStreamName,
                    embed::ElementModes::READ | embed::ElementModes::NOCREATE );

            // todo: encryption

            if( xInputStream.is())
            {
                sal_Int32 nArgs = 0;
                if( xGraphicStorageHandler.is())
                    nArgs++;
                if( xImportInfo.is())
                    nArgs++;

                cpo::uno::Sequence< cpo::uno::Any > aFilterCompArgs( nArgs );
                auto aFilterCompArgsRange = asNonConstRange(aFilterCompArgs);

                nArgs = 0;
                if( xGraphicStorageHandler.is())
                    aFilterCompArgsRange[nArgs++] <<= xGraphicStorageHandler;
                if( xImportInfo.is())
                    aFilterCompArgsRange[ nArgs++ ] <<= xImportInfo;

                // the underlying SvXMLImport implements XFastParser, XImporter, XFastDocumentHandler
                Reference< XInterface  > xFilter =
                    xFactory->createInstanceWithArgumentsAndContext( rServiceName, aFilterCompArgs, m_xContext );
                assert(xFilter);
                Reference< document::XImporter > xImporter( xFilter, uno::UNO_QUERY );
                assert(xImporter);
                xImporter->setTargetDocument( Reference< lang::XComponent >( m_xTargetDoc, uno::UNO_SET_THROW ));

                if ( !m_sDocumentHandler.isEmpty() )
                {
                    try
                    {
                        cpo::uno::Sequence< cpo::uno::Any > aArgs{
                            cpo::uno::Any(beans::NamedValue(u"DocumentHandler"_ustr, cpo::uno::Any(xFilter))),
                            cpo::uno::Any(beans::NamedValue(u"Model"_ustr, cpo::uno::Any(m_xTargetDoc)))
                        };

                        xFilter = xFactory->createInstanceWithArgumentsAndContext(m_sDocumentHandler,aArgs,m_xContext);
                    }
                    catch (const cpo::uno::Exception&)
                    {
                        TOOLS_WARN_EXCEPTION("chart2", "failed to instantiate " << m_sDocumentHandler);
                    }
                }
                xml::sax::InputSource aParserInput;
                aParserInput.aInputStream.set(xInputStream, uno::UNO_QUERY_THROW);

                // the underlying SvXMLImport implements XFastParser, XImporter, XFastDocumentHandler
                Reference< xml::sax::XFastParser > xFastParser(xFilter, uno::UNO_QUERY);
                if (xFastParser.is())
                    xFastParser->parseStream(aParserInput);
                else
                {
                    Reference<xml::sax::XParser> xParser = xml::sax::Parser::create(m_xContext);
                    xParser->setDocumentHandler( uno::Reference<xml::sax::XDocumentHandler>(xFilter, uno::UNO_QUERY_THROW) );
                    xParser->parseStream(aParserInput);
                }
            }

            // load was successful
            nWarning = ERRCODE_NONE;
        }
        catch (const xml::sax::SAXParseException&)
        {
            // todo: if encrypted: ERRCODE_SFX_WRONGPASSWORD
        }
        catch (const xml::sax::SAXException&)
        {
            // todo: if encrypted: ERRCODE_SFX_WRONGPASSWORD
        }
        catch (const packages::zip::ZipIOException&)
        {
            nWarning = ERRCODE_IO_BROKENPACKAGE;
        }
        catch (const io::IOException&)
        {
            TOOLS_WARN_EXCEPTION("chart2", "");
        }
        catch (const cpo::uno::Exception&)
        {
            DBG_UNHANDLED_EXCEPTION("chart2");
        }
    }

    return nWarning;
}

ErrCode XMLFilter::impl_Export(
    const Reference< lang::XComponent > & xDocumentComp,
    const Sequence< beans::PropertyValue > & rMediaDescriptor )
{
    m_aMediaDescriptor = rMediaDescriptor;
    //save

    ErrCode nWarning = ERRCODE_NONE;

    OSL_ENSURE( xDocumentComp.is(), "Export: No Model" );
    OSL_ENSURE( m_xContext.is(), "Export: No ComponentContext" );

    if( !xDocumentComp.is() || !m_xContext.is() )
        return nWarning;

    try
    {
        Reference< lang::XServiceInfo > xServInfo( xDocumentComp, uno::UNO_QUERY_THROW );
        if( ! xServInfo->supportsService( u"com.sun.star.chart2.ChartDocument"_ustr))
        {
            OSL_FAIL( "Export: No ChartDocument" );
            return ERRCODE_SFX_GENERAL;
        }

        Reference< lang::XMultiComponentFactory > xFactory( m_xContext->getServiceManager());
        OSL_ENSURE( xFactory.is(), "Export: No Factory" );
        if( ! xFactory.is())
            return ERRCODE_SFX_GENERAL;
        uno::Reference< lang::XMultiServiceFactory > xServiceFactory( m_xContext->getServiceManager(), uno::UNO_QUERY);
        if( ! xServiceFactory.is())
            return ERRCODE_SFX_GENERAL;

        uno::Reference< xml::sax::XWriter > xSaxWriter = xml::sax::Writer::create(m_xContext);

        bool bOasis = true;
        isOasisFormat( rMediaDescriptor, bOasis );

        uno::Reference< embed::XStorage > xStorage( lcl_getWriteStorage( rMediaDescriptor, m_xContext, getMediaType(bOasis) ) );
        OSL_ENSURE( xStorage.is(), "No Storage" );
        if( ! xStorage.is())
            return ERRCODE_SFX_GENERAL;

        uno::Reference< xml::sax::XDocumentHandler> xDocHandler = xSaxWriter;

        if ( !m_sDocumentHandler.isEmpty() )
        {
            try
            {
                cpo::uno::Sequence< cpo::uno::Any > aArgs{
                    cpo::uno::Any(beans::NamedValue(u"DocumentHandler"_ustr, cpo::uno::Any(xDocHandler))),
                    cpo::uno::Any(beans::NamedValue(u"Model"_ustr, cpo::uno::Any(xDocumentComp)))
                };

                xDocHandler.set(xServiceFactory->createInstanceWithArguments(m_sDocumentHandler,aArgs), uno::UNO_QUERY );
                xSaxWriter.set(xDocHandler,uno::UNO_QUERY);
            }
            catch (const cpo::uno::Exception&)
            {
                TOOLS_WARN_EXCEPTION( "chart2", "Exception caught!");
            }
        }

        Reference<document::XGraphicStorageHandler> xGraphicStorageHandler;
        xGraphicStorageHandler.set(document::GraphicStorageHandler::createWithStorage(m_xContext, xStorage));

        // property map for export info set
        static comphelper::PropertyMapEntry const aExportInfoMap[] =
        {
            { u"UsePrettyPrinting"_ustr, 0, cppu::UnoType<bool>::get(), beans::PropertyAttribute::MAYBEVOID, 0},
            { u"BaseURI"_ustr, 0, ::cppu::UnoType<OUString>::get(), beans::PropertyAttribute::MAYBEVOID, 0 },
            { u"StreamRelPath"_ustr, 0, ::cppu::UnoType<OUString>::get(), beans::PropertyAttribute::MAYBEVOID, 0 },
            { u"StreamName"_ustr, 0, ::cppu::UnoType<OUString>::get(), beans::PropertyAttribute::MAYBEVOID, 0 },
            { u"ExportTableNumberList"_ustr, 0, cppu::UnoType<bool>::get(), beans::PropertyAttribute::MAYBEVOID, 0 },
        };

        uno::Reference< beans::XPropertySet > xInfoSet =
            comphelper::GenericPropertySet_CreateInstance( new comphelper::PropertySetInfo( aExportInfoMap ) );

        bool bUsePrettyPrinting( officecfg::Office::Common::Save::Document::PrettyPrinting::get() );
        xInfoSet->setPropertyValue( u"UsePrettyPrinting"_ustr, cpo::uno::Any( bUsePrettyPrinting ) );
        if( ! bOasis )
            xInfoSet->setPropertyValue( u"ExportTableNumberList"_ustr, cpo::uno::Any( true ));

        sal_Int32 nArgs = 2;
        if( xGraphicStorageHandler.is())
            nArgs++;

        cpo::uno::Sequence< cpo::uno::Any > aFilterProperties( nArgs );
        {
            auto pFilterProperties = aFilterProperties.getArray();
            nArgs = 0;
            pFilterProperties[ nArgs++ ] <<= xInfoSet;
            pFilterProperties[ nArgs++ ] <<= xDocHandler;
            if( xGraphicStorageHandler.is())
                pFilterProperties[ nArgs++ ] <<= xGraphicStorageHandler;
        }

        // export meta information
        if( bOasis )
            nWarning = impl_ExportStream(
                sXML_metaStreamName,
                u"com.sun.star.comp.Chart.XMLOasisMetaExporter"_ustr,
                xStorage, xSaxWriter, xServiceFactory, aFilterProperties );

        // export styles
        ErrCode nTmp = impl_ExportStream(
            sXML_styleStreamName,
            bOasis
            ? u"com.sun.star.comp.Chart.XMLOasisStylesExporter"_ustr
            : u"com.sun.star.comp.Chart.XMLStylesExporter"_ustr, // soffice 6/7
            xStorage, xSaxWriter, xServiceFactory, aFilterProperties );
        nWarning = nWarning != ERRCODE_NONE ? nWarning : nTmp;

        // export content
        ErrCode nContentWarning = impl_ExportStream(
            sXML_contentStreamName,
            bOasis
            ? u"com.sun.star.comp.Chart.XMLOasisContentExporter"_ustr
            : u"com.sun.star.comp.Chart.XMLContentExporter"_ustr,
            xStorage, xSaxWriter, xServiceFactory, aFilterProperties );
        nWarning = nWarning != ERRCODE_NONE ? nWarning : nContentWarning;

        Reference< lang::XComponent > xComp(xGraphicStorageHandler, uno::UNO_QUERY);
        if (xComp.is())
            xComp->dispose();

        if (nWarning == ERRCODE_NONE)
            storeWorkbook(xDocumentComp, xStorage);

        uno::Reference<embed::XTransactedObject> xTransact( xStorage ,uno::UNO_QUERY);
        if ( xTransact.is() )
            xTransact->commit();
    }
    catch (const cpo::uno::Exception&)
    {
        DBG_UNHANDLED_EXCEPTION("chart2");

        // something went awry
        nWarning = ERRCODE_SFX_GENERAL;
    }

    return nWarning;
}

ErrCode XMLFilter::impl_ExportStream(
    const OUString & rStreamName,
    const OUString & rServiceName,
    const Reference< embed::XStorage > & xStorage,
    const uno::Reference< xml::sax::XWriter >& xActiveDataSource,
    const Reference< lang::XMultiServiceFactory >& xServiceFactory,
    const Sequence< cpo::uno::Any > & rFilterProperties )
{
    try
    {
        if( !xServiceFactory.is() )
            return ERRCODE_SFX_GENERAL;
        if( !xStorage.is() )
            return ERRCODE_SFX_GENERAL;
        if ( !xActiveDataSource.is() )
            return ERRCODE_SFX_GENERAL;

        uno::Reference< io::XStream > xStream( xStorage->openStreamElement(
            rStreamName, embed::ElementModes::READWRITE | embed::ElementModes::TRUNCATE ) );
        if ( !xStream.is() )
            return ERRCODE_SFX_GENERAL;
        uno::Reference< io::XOutputStream > xOutputStream( xStream->getOutputStream() );
        if ( !xOutputStream.is() )
            return ERRCODE_SFX_GENERAL;

        uno::Reference< beans::XPropertySet > xStreamProp( xOutputStream, uno::UNO_QUERY );
        if(xStreamProp.is()) try
        {
            xStreamProp->setPropertyValue( u"MediaType"_ustr, cpo::uno::Any( u"text/xml"_ustr ) );
            xStreamProp->setPropertyValue( u"Compressed"_ustr, cpo::uno::Any( true ) );//@todo?
            xStreamProp->setPropertyValue( u"UseCommonStoragePasswordEncryption"_ustr, cpo::uno::Any( true ) );
        }
        catch (const cpo::uno::Exception&)
        {
            DBG_UNHANDLED_EXCEPTION("chart2");
        }

        xActiveDataSource->setOutputStream(xOutputStream);

        // set Base URL
        {
            uno::Reference< beans::XPropertySet > xInfoSet;
            if( rFilterProperties.hasElements() )
                rFilterProperties[0] >>= xInfoSet;
            OSL_ENSURE( xInfoSet.is(), "missing infoset for export" );
            if( xInfoSet.is() )
                xInfoSet->setPropertyValue( u"StreamName"_ustr, cpo::uno::Any( rStreamName ) );
        }

        Reference< XExporter > xExporter( xServiceFactory->createInstanceWithArguments(
            rServiceName, rFilterProperties ), uno::UNO_QUERY);
        if ( !xExporter.is() )
            return ERRCODE_SFX_GENERAL;

        xExporter->setSourceDocument( m_xSourceDoc );

        uno::Reference< document::XFilter > xFilter( xExporter, uno::UNO_QUERY );
        if ( !xFilter.is() )
            return ERRCODE_SFX_GENERAL;

        xFilter->filter(m_aMediaDescriptor);
    }
    catch (const cpo::uno::Exception&)
    {
        DBG_UNHANDLED_EXCEPTION("chart2");
    }
    return ERRCODE_NONE;
}

void XMLFilter::isOasisFormat(const Sequence< beans::PropertyValue >& _rMediaDescriptor, bool & rOutOASIS )
{
    apphelper::MediaDescriptorHelper aMDHelper( _rMediaDescriptor );
    if( aMDHelper.ISSET_FilterName )
        rOutOASIS = aMDHelper.FilterName == "chart8";
}
OUString XMLFilter::getMediaType(bool _bOasis)
{
    return _bOasis ? MIMETYPE_OASIS_OPENDOCUMENT_CHART : MIMETYPE_VND_SUN_XML_CHART;
}

OUString XMLFilter::getImplementationName()
{
    return u"com.sun.star.comp.chart2.XMLFilter"_ustr;
}

bool XMLFilter::supportsService( const OUString& rServiceName )
{
    return cppu::supportsService(this, rServiceName);
}

cpo::uno::Sequence< OUString > XMLFilter::getSupportedServiceNames()
{
    return {
        u"com.sun.star.document.ImportFilter"_ustr,
        u"com.sun.star.document.ExportFilter"_ustr
    };
    // todo: services are incomplete.  Missing:
    // XInitialization, XNamed
}

void XMLReportFilterHelper::isOasisFormat(const Sequence< beans::PropertyValue >& _rMediaDescriptor, bool & rOutOASIS )
{
    apphelper::MediaDescriptorHelper aMDHelper( _rMediaDescriptor );
    if( aMDHelper.ISSET_FilterName )
        rOutOASIS = aMDHelper.FilterName == "StarOffice XML (Base) Report Chart";
}
OUString XMLReportFilterHelper::getMediaType(bool )
{
    return MIMETYPE_OASIS_OPENDOCUMENT_REPORT_CHART;
}

} //  namespace chart

extern "C" SAL_DLLPUBLIC_EXPORT cpo::uno::XInterface *
com_sun_star_comp_chart2_XMLFilter_get_implementation(cpo::uno::XComponentContext *context,
        cpo::uno::Sequence<cpo::uno::Any> const &)
{
    return cppu::acquire(new ::chart::XMLFilter(context));
}

extern "C" SAL_DLLPUBLIC_EXPORT cpo::uno::XInterface *
com_sun_star_comp_chart2_report_XMLFilter_get_implementation(cpo::uno::XComponentContext *context,
        cpo::uno::Sequence<cpo::uno::Any> const &)
{
    return cppu::acquire(new ::chart::XMLReportFilterHelper(context));
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
