/***************************************************************************
                              qgsserverprojectparser.cpp
                              --------------------------
  begin                : March 25, 2014
  copyright            : (C) 2014 by Marco Hugentobler
  email                : marco dot hugentobler at sourcepole dot ch
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsserverprojectparser.h"
#include "qgsapplication.h"
#include "qgsproject.h"
#include "qgsconfigcache.h"
#include "qgsconfigparserutils.h"
#include "qgsexception.h"
#include "qgsdatasourceuri.h"
#include "qgsmslayercache.h"
#include "qgspathresolver.h"
#include "qgsrasterlayer.h"
#include "qgsreadwritecontext.h"
#include "qgsvectorlayerjoinbuffer.h"
#include "qgslayertreegroup.h"
#include "qgslayertreelayer.h"
#include "qgslayertree.h"
#include "qgslogger.h"
#include "qgseditorwidgetsetup.h"
#include "qgsexpressionnodeimpl.h"
#include "qgsserverprojectutils.h"

#include <QDomDocument>
#include <QFileInfo>
#include <QStringList>
#include <QTextStream>
#include <QUrl>

QgsServerProjectParser::QgsServerProjectParser( QDomDocument *xmlDoc, const QString &filePath )
  : mXMLDoc( xmlDoc )
  , mProject( QgsConfigCache::instance()->project( filePath ) )
  , mProjectPath( filePath )
{
  QMap<QString, QgsMapLayer *> layers = mProject->mapLayers();
  mProjectLayerElements.reserve( layers.size() );
  Q_FOREACH ( QgsMapLayer *layer, layers )
  {
    QDomDocument doc;
    QDomElement el = doc.createElement( "maplayer" );
    layer->writeLayerXml( el, doc, QgsReadWriteContext() );
    mProjectLayerElements.push_back( el );

    QString name = layer->shortName();
    if ( name.isEmpty() )
    {
      name = layer->name();
    }

    mProjectLayerElementsByName.insert( name, el );
    mProjectLayerElementsById.insert( layer->id(), el );
  }

  mRestrictedLayers = findRestrictedLayers();

  //accelerate the search for layers, groups and the creation of annotation items
  if ( mXMLDoc )
  {
    mLegendGroupElements = findLegendGroupElements();

    mCustomLayerOrder.clear();

    QDomElement customOrder = mXMLDoc->documentElement().firstChildElement( QStringLiteral( "layer-tree-canvas" ) ).firstChildElement( QStringLiteral( "custom-order" ) );
    if ( customOrder.attribute( QStringLiteral( "enabled" ) ) == QLatin1String( "1" ) )
    {
      QDomNodeList items = customOrder.childNodes();
      for ( int i = 0; i < items.size(); ++i )
      {
        mCustomLayerOrder << items.item( i ).toElement().text();
      }
    }
  }
  // Setting the QgsProject instance fileName
  // to help converting relative paths to absolute
  if ( !mProjectPath.isEmpty() )
  {
    QgsProject::instance()->setFileName( mProjectPath );
  }
}

bool QgsServerProjectParser::useLayerIds() const
{
  return QgsServerProjectUtils::wmsUseLayerIds( *mProject );
}

QStringList QgsServerProjectParser::layersNames() const
{
  QStringList names;
  Q_FOREACH ( QgsMapLayer *layer, mProject->mapLayers() )
  {
    if ( ! layer->shortName().isEmpty() )
    {
      names.append( layer->shortName() );
    }
    else
    {
      names.append( layer->name() );
    }
  }

  return names;
}

QgsServerProjectParser::QgsServerProjectParser()
  : mXMLDoc( nullptr )
{
}

void QgsServerProjectParser::projectLayerMap( QMap<QString, QgsMapLayer *> &layerMap ) const
{
  layerMap.clear();

  QList<QDomElement>::const_iterator layerElemIt = mProjectLayerElements.constBegin();
  for ( ; layerElemIt != mProjectLayerElements.constEnd(); ++layerElemIt )
  {
    QgsMapLayer *layer = createLayerFromElement( *layerElemIt );
    if ( layer )
    {
      layerMap.insert( layer->id(), layer );
    }
  }
}

QString QgsServerProjectParser::convertToAbsolutePath( const QString &file ) const
{
  if ( !file.startsWith( QLatin1String( "./" ) ) && !file.startsWith( QLatin1String( "../" ) ) )
  {
    return file;
  }

  QString srcPath = file;
  QString projPath = mProjectPath;

#if defined(Q_OS_WIN)
  srcPath.replace( "\\", "/" );
  projPath.replace( "\\", "/" );

  bool uncPath = projPath.startsWith( "//" );
#endif

  QStringList srcElems = srcPath.split( QStringLiteral( "/" ), QString::SkipEmptyParts );
  QStringList projElems = projPath.split( QStringLiteral( "/" ), QString::SkipEmptyParts );

#if defined(Q_OS_WIN)
  if ( uncPath )
  {
    projElems.prepend( "" );
    projElems.prepend( "" );
  }
#endif

  // remove project file element
  projElems.removeLast();

  // append source path elements
  projElems << srcElems;
  projElems.removeAll( QStringLiteral( "." ) );

  // resolve ..
  int pos;
  while ( ( pos = projElems.indexOf( QStringLiteral( ".." ) ) ) > 0 )
  {
    // remove preceding element and ..
    projElems.removeAt( pos - 1 );
    projElems.removeAt( pos - 1 );
  }

#if !defined(Q_OS_WIN)
  // make path absolute
  projElems.prepend( QLatin1String( "" ) );
#endif

  return projElems.join( QStringLiteral( "/" ) );
}

QgsMapLayer *QgsServerProjectParser::createLayerFromElement( const QDomElement &elem, bool useCache ) const
{
  if ( elem.isNull() || !mXMLDoc )
  {
    return nullptr;
  }

  addJoinLayersForElement( elem );
  addGetFeatureLayers( elem );

  QDomElement dataSourceElem = elem.firstChildElement( QStringLiteral( "datasource" ) );
  QString uri = dataSourceElem.text();
  QString absoluteUri;
  // If QgsProject instance fileName is set,
  // Is converting relative paths to absolute still relevant ?
  if ( !dataSourceElem.isNull() )
  {
    //convert relative paths to absolute ones if necessary
    if ( uri.startsWith( QLatin1String( "dbname" ) ) ) //database
    {
      QgsDataSourceUri dsUri( uri );
      if ( dsUri.host().isEmpty() ) //only convert path for file based databases
      {
        QString dbnameUri = dsUri.database();
        QString dbNameUriAbsolute = convertToAbsolutePath( dbnameUri );
        if ( dbnameUri != dbNameUriAbsolute )
        {
          dsUri.setDatabase( dbNameUriAbsolute );
          absoluteUri = dsUri.uri();
          QDomText absoluteTextNode = mXMLDoc->createTextNode( absoluteUri );
          dataSourceElem.replaceChild( absoluteTextNode, dataSourceElem.firstChild() );
        }
      }
    }
    else if ( uri.startsWith( QLatin1String( "file:" ) ) ) //a file based datasource in url notation (e.g. delimited text layer)
    {
      QString filePath = uri.mid( 5, uri.indexOf( QLatin1String( "?" ) ) - 5 );
      QString absoluteFilePath = convertToAbsolutePath( filePath );
      if ( filePath != absoluteFilePath )
      {
        QUrl destUrl = QUrl::fromEncoded( uri.toLatin1() );
        destUrl.setScheme( QStringLiteral( "file" ) );
        destUrl.setPath( absoluteFilePath );
        absoluteUri = destUrl.toEncoded();
        QDomText absoluteTextNode = mXMLDoc->createTextNode( absoluteUri );
        dataSourceElem.replaceChild( absoluteTextNode, dataSourceElem.firstChild() );
      }
      else
      {
        absoluteUri = uri;
      }
    }
    else //file based data source
    {
      absoluteUri = convertToAbsolutePath( uri );
      if ( uri != absoluteUri )
      {
        QDomText absoluteTextNode = mXMLDoc->createTextNode( absoluteUri );
        dataSourceElem.replaceChild( absoluteTextNode, dataSourceElem.firstChild() );
      }
    }
  }

  QString id = layerId( elem );
  QgsMapLayer *layer = nullptr;
  if ( useCache )
  {
    layer = QgsMSLayerCache::instance()->searchLayer( absoluteUri, id, mProjectPath );
  }

  if ( layer )
  {
    if ( !QgsProject::instance()->mapLayer( id ) )
      QgsProject::instance()->addMapLayer( layer, false, false );
    if ( layer->type() == QgsMapLayer::VectorLayer )
    {
      QgsVectorLayer *vlayer = qobject_cast<QgsVectorLayer *>( layer );
      addValueRelationLayersForLayer( vlayer );
      QgsVectorLayerJoinBuffer *joinBuffer = vlayer->joinBuffer();
      joinBuffer->readXml( const_cast<QDomElement &>( elem ) );
      joinBuffer->resolveReferences( QgsProject::instance() );
    }

    return layer;
  }

  QString type = elem.attribute( QStringLiteral( "type" ) );
  if ( type == QLatin1String( "vector" ) )
  {
    layer = new QgsVectorLayer();
  }
  else if ( type == QLatin1String( "raster" ) )
  {
    layer = new QgsRasterLayer();
  }
  else if ( elem.attribute( QStringLiteral( "embedded" ) ) == QLatin1String( "1" ) ) //layer is embedded from another project file
  {
    QString project = convertToAbsolutePath( elem.attribute( QStringLiteral( "project" ) ) );
    QgsDebugMsg( QString( "Project path: %1" ).arg( project ) );

    QgsServerProjectParser *otherConfig = QgsConfigCache::instance()->serverConfiguration( project );
    if ( !otherConfig )
    {
      return nullptr;
    }
    return otherConfig->mapLayerFromLayerId( elem.attribute( QStringLiteral( "id" ) ), useCache );
  }

  if ( layer )
  {
    QgsReadWriteContext context;
    context.setPathResolver( QgsProject::instance()->pathResolver() );

    layer->readLayerXml( const_cast<QDomElement &>( elem ), context ); //should be changed to const in QgsMapLayer
    //layer->setLayerName( layerName( elem ) );

    if ( !layer->isValid() )
    {
      return nullptr;
    }
    // Insert layer in registry and cache before addValueRelationLayersForLayer
    if ( !QgsProject::instance()->mapLayer( id ) )
      QgsProject::instance()->addMapLayer( layer, false, false );
    if ( useCache )
    {
      QgsMSLayerCache::instance()->insertLayer( absoluteUri, id, layer, mProjectPath );
    }
    else
    {
      //todo: fixme
      //mLayersToRemove.push_back( layer );
    }

    if ( layer->type() == QgsMapLayer::VectorLayer )
    {
      addValueRelationLayersForLayer( qobject_cast<QgsVectorLayer *>( layer ) );
    }
  }
  return layer;
}

QgsMapLayer *QgsServerProjectParser::mapLayerFromLayerId( const QString &lId, bool useCache ) const
{
  QHash< QString, QDomElement >::const_iterator layerIt = mProjectLayerElementsById.find( lId );
  if ( layerIt != mProjectLayerElementsById.constEnd() )
  {
    return createLayerFromElement( layerIt.value(), useCache );
  }
  return nullptr;
}

QString QgsServerProjectParser::layerIdFromLegendLayer( const QDomElement &legendLayer ) const
{
  if ( legendLayer.isNull() )
  {
    return QString();
  }

  QDomNodeList legendLayerFileList = legendLayer.elementsByTagName( QStringLiteral( "legendlayerfile" ) );
  if ( legendLayerFileList.size() < 1 )
  {
    return QString();
  }
  return legendLayerFileList.at( 0 ).toElement().attribute( QStringLiteral( "layerid" ) );
}

QString QgsServerProjectParser::layerId( const QDomElement &layerElem ) const
{
  if ( layerElem.isNull() )
  {
    return QString();
  }

  QDomElement idElem = layerElem.firstChildElement( QStringLiteral( "id" ) );
  if ( idElem.isNull() )
  {
    //embedded layer have id attribute instead of id child element
    return layerElem.attribute( QStringLiteral( "id" ) );
  }
  return idElem.text();
}

QgsRectangle QgsServerProjectParser::projectExtent() const
{
  QgsRectangle extent;
  if ( !mXMLDoc )
  {
    return extent;
  }

  QDomElement qgisElem = mXMLDoc->documentElement();
  QDomElement mapCanvasElem = qgisElem.firstChildElement( QStringLiteral( "mapcanvas" ) );
  if ( mapCanvasElem.isNull() )
  {
    return extent;
  }

  QDomElement extentElem = mapCanvasElem.firstChildElement( QStringLiteral( "extent" ) );
  bool xminOk, xmaxOk, yminOk, ymaxOk;
  double xMin = extentElem.firstChildElement( QStringLiteral( "xmin" ) ).text().toDouble( &xminOk );
  double xMax = extentElem.firstChildElement( QStringLiteral( "xmax" ) ).text().toDouble( &xmaxOk );
  double yMin = extentElem.firstChildElement( QStringLiteral( "ymin" ) ).text().toDouble( &yminOk );
  double yMax = extentElem.firstChildElement( QStringLiteral( "ymax" ) ).text().toDouble( &ymaxOk );

  if ( xminOk && xmaxOk && yminOk && ymaxOk )
  {
    extent = QgsRectangle( xMin, yMin, xMax, yMax );
  }

  return extent;
}

int QgsServerProjectParser::numberOfLayers() const
{
  return mProjectLayerElements.size();
}

bool QgsServerProjectParser::updateLegendDrawingOrder() const
{
  return !mCustomLayerOrder.isEmpty();
}

void QgsServerProjectParser::serviceCapabilities( QDomElement &parentElement, QDomDocument &doc, const QString &service, bool sia2045 ) const
{
  QDomElement propertiesElement = propertiesElem();
  if ( propertiesElement.isNull() )
  {
    QgsConfigParserUtils::fallbackServiceCapabilities( parentElement, doc );
    return;
  }
  QDomElement serviceElem = doc.createElement( QStringLiteral( "Service" ) );

  QDomElement serviceCapabilityElem = propertiesElement.firstChildElement( QStringLiteral( "WMSServiceCapabilities" ) );
  if ( serviceCapabilityElem.isNull() || serviceCapabilityElem.text().compare( QLatin1String( "true" ), Qt::CaseInsensitive ) != 0 )
  {
    QgsConfigParserUtils::fallbackServiceCapabilities( parentElement, doc );
    return;
  }

  //Service name
  QDomElement wmsNameElem = doc.createElement( QStringLiteral( "Name" ) );
  QDomText wmsNameText = doc.createTextNode( service );
  wmsNameElem.appendChild( wmsNameText );
  serviceElem.appendChild( wmsNameElem );

  //WMS title
  //why not use project title ?
  QDomElement titleElem = propertiesElement.firstChildElement( QStringLiteral( "WMSServiceTitle" ) );
  if ( !titleElem.isNull() )
  {
    QDomElement wmsTitleElem = doc.createElement( QStringLiteral( "Title" ) );
    QDomText wmsTitleText = doc.createTextNode( titleElem.text() );
    wmsTitleElem.appendChild( wmsTitleText );
    serviceElem.appendChild( wmsTitleElem );
  }

  //WMS abstract
  QDomElement abstractElem = propertiesElement.firstChildElement( QStringLiteral( "WMSServiceAbstract" ) );
  if ( !abstractElem.isNull() )
  {
    QDomElement wmsAbstractElem = doc.createElement( QStringLiteral( "Abstract" ) );
    QDomText wmsAbstractText = doc.createTextNode( abstractElem.text() );
    wmsAbstractElem.appendChild( wmsAbstractText );
    serviceElem.appendChild( wmsAbstractElem );
  }

  //keyword list
  QDomElement keywordListElem = propertiesElement.firstChildElement( QStringLiteral( "WMSKeywordList" ) );
  if ( service.compare( QLatin1String( "WMS" ), Qt::CaseInsensitive ) == 0 )
  {
    QDomElement wmsKeywordElem = doc.createElement( QStringLiteral( "KeywordList" ) );
    //add default keyword
    QDomElement keywordElem = doc.createElement( QStringLiteral( "Keyword" ) );
    keywordElem.setAttribute( QStringLiteral( "vocabulary" ), QStringLiteral( "ISO" ) );
    QDomText keywordText = doc.createTextNode( QStringLiteral( "infoMapAccessService" ) );
    /* If WFS and WCS 2.0 is implemented
    if ( service.compare( "WFS", Qt::CaseInsensitive ) == 0 )
      keywordText = doc.createTextNode( "infoFeatureAccessService" );
    else if ( service.compare( "WCS", Qt::CaseInsensitive ) == 0 )
      keywordText = doc.createTextNode( "infoCoverageAccessService" );*/
    keywordElem.appendChild( keywordText );
    wmsKeywordElem.appendChild( keywordElem );
    serviceElem.appendChild( wmsKeywordElem );
    //add config keywords
    if ( !keywordListElem.isNull() && !keywordListElem.text().isEmpty() )
    {
      QDomNodeList keywordList = keywordListElem.elementsByTagName( QStringLiteral( "value" ) );
      for ( int i = 0; i < keywordList.size(); ++i )
      {
        keywordElem = doc.createElement( QStringLiteral( "Keyword" ) );
        keywordText = doc.createTextNode( keywordList.at( i ).toElement().text() );
        keywordElem.appendChild( keywordText );
        if ( sia2045 )
        {
          keywordElem.setAttribute( QStringLiteral( "vocabulary" ), QStringLiteral( "SIA_Geo405" ) );
        }
        wmsKeywordElem.appendChild( keywordElem );
      }
    }
  }
  else if ( !keywordListElem.isNull() && !keywordListElem.text().isEmpty() )
  {
    QDomNodeList keywordNodeList = keywordListElem.elementsByTagName( QStringLiteral( "value" ) );
    QStringList keywordList;
    for ( int i = 0; i < keywordNodeList.size(); ++i )
    {
      keywordList.push_back( keywordNodeList.at( i ).toElement().text() );
    }
    QDomElement wmsKeywordElem = doc.createElement( QStringLiteral( "Keywords" ) );
    if ( service.compare( QLatin1String( "WCS" ), Qt::CaseInsensitive ) == 0 )
      wmsKeywordElem = doc.createElement( QStringLiteral( "keywords" ) );
    QDomText keywordText = doc.createTextNode( keywordList.join( QStringLiteral( ", " ) ) );
    wmsKeywordElem.appendChild( keywordText );
    serviceElem.appendChild( wmsKeywordElem );
  }

  //OnlineResource element is mandatory according to the WMS specification
  QDomElement wmsOnlineResourceElem = propertiesElement.firstChildElement( QStringLiteral( "WMSOnlineResource" ) );
  if ( !wmsOnlineResourceElem.isNull() )
  {
    QDomElement onlineResourceElem = doc.createElement( QStringLiteral( "OnlineResource" ) );
    if ( service.compare( QLatin1String( "WFS" ), Qt::CaseInsensitive ) == 0 )
    {
      QDomText onlineResourceText = doc.createTextNode( wmsOnlineResourceElem.text() );
      onlineResourceElem.appendChild( onlineResourceText );
    }
    else
    {
      onlineResourceElem.setAttribute( QStringLiteral( "xmlns:xlink" ), QStringLiteral( "http://www.w3.org/1999/xlink" ) );
      onlineResourceElem.setAttribute( QStringLiteral( "xlink:type" ), QStringLiteral( "simple" ) );
      onlineResourceElem.setAttribute( QStringLiteral( "xlink:href" ), wmsOnlineResourceElem.text() );
    }
    serviceElem.appendChild( onlineResourceElem );
  }

  if ( service.compare( QLatin1String( "WMS" ), Qt::CaseInsensitive ) == 0 ) //no contact information in WFS 1.0 and WCS 1.0
  {
    //Contact information
    QDomElement contactInfoElem = doc.createElement( QStringLiteral( "ContactInformation" ) );

    //Contact person primary
    QDomElement contactPersonPrimaryElem = doc.createElement( QStringLiteral( "ContactPersonPrimary" ) );

    //Contact person
    QDomElement contactPersonElem = propertiesElement.firstChildElement( QStringLiteral( "WMSContactPerson" ) );
    QString contactPersonString;
    if ( !contactPersonElem.isNull() )
    {
      contactPersonString = contactPersonElem.text();
    }
    QDomElement wmsContactPersonElem = doc.createElement( QStringLiteral( "ContactPerson" ) );
    QDomText contactPersonText = doc.createTextNode( contactPersonString );
    wmsContactPersonElem.appendChild( contactPersonText );
    contactPersonPrimaryElem.appendChild( wmsContactPersonElem );


    //Contact organisation
    QDomElement contactOrganizationElem = propertiesElement.firstChildElement( QStringLiteral( "WMSContactOrganization" ) );
    QString contactOrganizationString;
    if ( !contactOrganizationElem.isNull() )
    {
      contactOrganizationString = contactOrganizationElem.text();
    }
    QDomElement wmsContactOrganizationElem = doc.createElement( QStringLiteral( "ContactOrganization" ) );
    QDomText contactOrganizationText = doc.createTextNode( contactOrganizationString );
    wmsContactOrganizationElem.appendChild( contactOrganizationText );
    contactPersonPrimaryElem.appendChild( wmsContactOrganizationElem );

    //Contact position
    QDomElement contactPositionElem = propertiesElement.firstChildElement( QStringLiteral( "WMSContactPosition" ) );
    QString contactPositionString;
    if ( !contactPositionElem.isNull() )
    {
      contactPositionString = contactPositionElem.text();
    }
    QDomElement wmsContactPositionElem = doc.createElement( QStringLiteral( "ContactPosition" ) );
    QDomText contactPositionText = doc.createTextNode( contactPositionString );
    wmsContactPositionElem.appendChild( contactPositionText );
    contactPersonPrimaryElem.appendChild( wmsContactPositionElem );
    contactInfoElem.appendChild( contactPersonPrimaryElem );

    //phone
    QDomElement phoneElem = propertiesElement.firstChildElement( QStringLiteral( "WMSContactPhone" ) );
    if ( !phoneElem.isNull() )
    {
      QDomElement wmsPhoneElem = doc.createElement( QStringLiteral( "ContactVoiceTelephone" ) );
      QDomText wmsPhoneText = doc.createTextNode( phoneElem.text() );
      wmsPhoneElem.appendChild( wmsPhoneText );
      contactInfoElem.appendChild( wmsPhoneElem );
    }

    //mail
    QDomElement mailElem = propertiesElement.firstChildElement( QStringLiteral( "WMSContactMail" ) );
    if ( !mailElem.isNull() )
    {
      QDomElement wmsMailElem = doc.createElement( QStringLiteral( "ContactElectronicMailAddress" ) );
      QDomText wmsMailText = doc.createTextNode( mailElem.text() );
      wmsMailElem.appendChild( wmsMailText );
      contactInfoElem.appendChild( wmsMailElem );
    }

    serviceElem.appendChild( contactInfoElem );
  }

  //Fees
  QDomElement feesElem = propertiesElement.firstChildElement( QStringLiteral( "WMSFees" ) );
  QDomElement wmsFeesElem = doc.createElement( QStringLiteral( "Fees" ) );
  QDomText wmsFeesText = doc.createTextNode( QStringLiteral( "None" ) ); // default value if access conditions are unknown
  if ( !feesElem.isNull() && !feesElem.text().isEmpty() )
  {
    wmsFeesText = doc.createTextNode( feesElem.text() );
  }
  wmsFeesElem.appendChild( wmsFeesText );
  serviceElem.appendChild( wmsFeesElem );

  //AccessConstraints
  QDomElement accessConstraintsElem = propertiesElement.firstChildElement( QStringLiteral( "WMSAccessConstraints" ) );
  QDomElement wmsAccessConstraintsElem = doc.createElement( QStringLiteral( "AccessConstraints" ) );
  QDomText wmsAccessConstraintsText = doc.createTextNode( QStringLiteral( "None" ) ); // default value if access constraints are unknown
  if ( !accessConstraintsElem.isNull() && !accessConstraintsElem.text().isEmpty() )
  {
    wmsAccessConstraintsText = doc.createTextNode( accessConstraintsElem.text() );
  }
  wmsAccessConstraintsElem.appendChild( wmsAccessConstraintsText );
  serviceElem.appendChild( wmsAccessConstraintsElem );

  //max width, max height for WMS
  if ( service.compare( QLatin1String( "WMS" ), Qt::CaseInsensitive ) == 0 )
  {
    QString version = doc.documentElement().attribute( QStringLiteral( "version" ) );
    if ( version != QLatin1String( "1.1.1" ) )
    {
      //max width
      QDomElement mwElem = propertiesElement.firstChildElement( QStringLiteral( "WMSMaxWidth" ) );
      if ( !mwElem.isNull() )
      {
        QDomElement maxWidthElem = doc.createElement( QStringLiteral( "MaxWidth" ) );
        QDomText maxWidthText = doc.createTextNode( mwElem.text() );
        maxWidthElem.appendChild( maxWidthText );
        serviceElem.appendChild( maxWidthElem );
      }
      //max height
      QDomElement mhElem = propertiesElement.firstChildElement( QStringLiteral( "WMSMaxHeight" ) );
      if ( !mhElem.isNull() )
      {
        QDomElement maxHeightElem = doc.createElement( QStringLiteral( "MaxHeight" ) );
        QDomText maxHeightText = doc.createTextNode( mhElem.text() );
        maxHeightElem.appendChild( maxHeightText );
        serviceElem.appendChild( maxHeightElem );
      }
    }
  }
  parentElement.appendChild( serviceElem );
}

void QgsServerProjectParser::combineExtentAndCrsOfGroupChildren( QDomElement &groupElem, QDomDocument &doc, bool considerMapExtent ) const
{
  QgsRectangle combinedBBox;
  QSet<QString> combinedCRSSet;
  bool firstBBox = true;
  bool firstCRSSet = true;

  QDomNodeList layerChildren = groupElem.childNodes();
  for ( int j = 0; j < layerChildren.size(); ++j )
  {
    QDomElement childElem = layerChildren.at( j ).toElement();

    if ( childElem.tagName() != QLatin1String( "Layer" ) )
      continue;

    QgsRectangle bbox = layerBoundingBoxInProjectCrs( childElem, doc );
    if ( bbox.isNull() )
    {
      continue;
    }

    if ( !bbox.isEmpty() )
    {
      if ( firstBBox )
      {
        combinedBBox = bbox;
        firstBBox = false;
      }
      else
      {
        combinedBBox.combineExtentWith( bbox );
      }
    }

    //combine crs set
    QSet<QString> crsSet;
    if ( crsSetForLayer( childElem, crsSet ) )
    {
      if ( firstCRSSet )
      {
        combinedCRSSet = crsSet;
        firstCRSSet = false;
      }
      else
      {
        combinedCRSSet.intersect( crsSet );
      }
    }
  }

  QgsConfigParserUtils::appendCrsElementsToLayer( groupElem, doc, combinedCRSSet.toList(), supportedOutputCrsList() );

  QgsCoordinateReferenceSystem groupCRS = projectCrs();
  if ( considerMapExtent )
  {
    QgsRectangle mapRect = mapRectangle();
    if ( !mapRect.isEmpty() )
    {
      combinedBBox = mapRect;
    }
  }
  QgsConfigParserUtils::appendLayerBoundingBoxes( groupElem, doc, combinedBBox, groupCRS, combinedCRSSet.toList(), supportedOutputCrsList() );
}

void QgsServerProjectParser::addLayerProjectSettings( QDomElement &layerElem, QDomDocument &doc, QgsMapLayer *currentLayer ) const
{
  if ( !currentLayer )
  {
    return;
  }
  // Layer tree name
  QDomElement treeNameElem = doc.createElement( QStringLiteral( "TreeName" ) );
  QDomText treeNameText = doc.createTextNode( currentLayer->name() );
  treeNameElem.appendChild( treeNameText );
  layerElem.appendChild( treeNameElem );

  if ( currentLayer->type() == QgsMapLayer::VectorLayer )
  {
    QgsVectorLayer *vLayer = static_cast<QgsVectorLayer *>( currentLayer );
    const QSet<QString> &excludedAttributes = vLayer->excludeAttributesWms();

    int displayFieldIdx = -1;
    QString displayField = QStringLiteral( "maptip" );
    QgsExpression exp( vLayer->displayExpression() );
    if ( exp.isField() )
    {
      displayField = static_cast<const QgsExpressionNodeColumnRef *>( exp.rootNode() )->name();
      displayFieldIdx = vLayer->fields().lookupField( displayField );
    }

    //attributes
    QDomElement attributesElem = doc.createElement( QStringLiteral( "Attributes" ) );
    const QgsFields &layerFields = vLayer->pendingFields();
    for ( int idx = 0; idx < layerFields.count(); ++idx )
    {
      QgsField field = layerFields.at( idx );
      if ( excludedAttributes.contains( field.name() ) )
      {
        continue;
      }
      // field alias in case of displayField
      if ( idx == displayFieldIdx )
      {
        displayField = vLayer->attributeDisplayName( idx );
      }
      QDomElement attributeElem = doc.createElement( QStringLiteral( "Attribute" ) );
      attributeElem.setAttribute( QStringLiteral( "name" ), field.name() );
      attributeElem.setAttribute( QStringLiteral( "type" ), QVariant::typeToName( field.type() ) );
      attributeElem.setAttribute( QStringLiteral( "typeName" ), field.typeName() );
      QString alias = field.alias();
      if ( !alias.isEmpty() )
      {
        attributeElem.setAttribute( QStringLiteral( "alias" ), alias );
      }

      //edit type to text
      attributeElem.setAttribute( QStringLiteral( "editType" ), vLayer->editorWidgetSetup( idx ).type() );
      attributeElem.setAttribute( QStringLiteral( "comment" ), field.comment() );
      attributeElem.setAttribute( QStringLiteral( "length" ), field.length() );
      attributeElem.setAttribute( QStringLiteral( "precision" ), field.precision() );
      attributesElem.appendChild( attributeElem );
    }
    //displayfield
    layerElem.setAttribute( QStringLiteral( "displayField" ), displayField );

    //geometry type
    layerElem.setAttribute( QStringLiteral( "geometryType" ), QgsWkbTypes::displayString( vLayer->wkbType() ) );

    layerElem.appendChild( attributesElem );
  }
}

QgsRectangle QgsServerProjectParser::layerBoundingBoxInProjectCrs( const QDomElement &layerElem, const QDomDocument &doc ) const
{
  QgsRectangle BBox;
  if ( layerElem.isNull() )
  {
    return BBox;
  }

  //read box coordinates and layer auth. id
  QDomElement boundingBoxElem = layerElem.firstChildElement( QStringLiteral( "BoundingBox" ) );
  if ( boundingBoxElem.isNull() )
  {
    return BBox;
  }

  double minx, miny, maxx, maxy;
  bool conversionOk;
  minx = boundingBoxElem.attribute( QStringLiteral( "minx" ) ).toDouble( &conversionOk );
  if ( !conversionOk )
  {
    return BBox;
  }
  miny = boundingBoxElem.attribute( QStringLiteral( "miny" ) ).toDouble( &conversionOk );
  if ( !conversionOk )
  {
    return BBox;
  }
  maxx = boundingBoxElem.attribute( QStringLiteral( "maxx" ) ).toDouble( &conversionOk );
  if ( !conversionOk )
  {
    return BBox;
  }
  maxy = boundingBoxElem.attribute( QStringLiteral( "maxy" ) ).toDouble( &conversionOk );
  if ( !conversionOk )
  {
    return BBox;
  }


  QString version = doc.documentElement().attribute( QStringLiteral( "version" ) );

  //create layer crs
  QgsCoordinateReferenceSystem layerCrs = QgsCoordinateReferenceSystem::fromOgcWmsCrs( boundingBoxElem.attribute( version == QLatin1String( "1.1.1" ) ? "SRS" : "CRS" ) );
  if ( !layerCrs.isValid() )
  {
    return BBox;
  }

  BBox.setXMinimum( minx );
  BBox.setXMaximum( maxx );
  BBox.setYMinimum( miny );
  BBox.setYMaximum( maxy );

  if ( version != QLatin1String( "1.1.1" ) && layerCrs.hasAxisInverted() )
  {
    BBox.invert();
  }

  //get project crs
  QgsCoordinateTransform t( layerCrs, projectCrs() );

  //transform
  try
  {
    BBox = t.transformBoundingBox( BBox );
  }
  catch ( const QgsCsException & )
  {
    BBox = QgsRectangle();
  }

  return BBox;
}

bool QgsServerProjectParser::crsSetForLayer( const QDomElement &layerElement, QSet<QString> &crsSet ) const
{
  if ( layerElement.isNull() )
  {
    return false;
  }

  crsSet.clear();

  QDomNodeList crsNodeList;
  crsNodeList = layerElement.elementsByTagName( QStringLiteral( "CRS" ) ); // WMS 1.3.0
  for ( int i = 0; i < crsNodeList.size(); ++i )
  {
    crsSet.insert( crsNodeList.at( i ).toElement().text() );
  }

  crsNodeList = layerElement.elementsByTagName( QStringLiteral( "SRS" ) ); // WMS 1.1.1
  for ( int i = 0; i < crsNodeList.size(); ++i )
  {
    crsSet.insert( crsNodeList.at( i ).toElement().text() );
  }

  return true;
}

QgsCoordinateReferenceSystem QgsServerProjectParser::projectCrs() const
{
  //mapcanvas->destinationsrs->spatialrefsys->authid
  if ( mXMLDoc )
  {
    QDomElement authIdElem = mXMLDoc->documentElement().firstChildElement( QStringLiteral( "mapcanvas" ) ).firstChildElement( QStringLiteral( "destinationsrs" ) ).
                             firstChildElement( QStringLiteral( "spatialrefsys" ) ).firstChildElement( QStringLiteral( "authid" ) );
    if ( !authIdElem.isNull() )
    {
      return QgsCoordinateReferenceSystem::fromOgcWmsCrs( authIdElem.text() );
    }
  }
  return QgsCoordinateReferenceSystem::fromEpsgId( GEO_EPSG_CRS_ID );
}

QgsRectangle QgsServerProjectParser::mapRectangle() const
{
  if ( !mXMLDoc )
  {
    return QgsRectangle();
  }

  QDomElement qgisElem = mXMLDoc->documentElement();
  if ( qgisElem.isNull() )
  {
    return QgsRectangle();
  }

  QDomElement propertiesElem = qgisElem.firstChildElement( QStringLiteral( "properties" ) );
  if ( propertiesElem.isNull() )
  {
    return QgsRectangle();
  }

  QDomElement extentElem = propertiesElem.firstChildElement( QStringLiteral( "WMSExtent" ) );
  if ( extentElem.isNull() )
  {
    return QgsRectangle();
  }

  QDomNodeList valueNodeList = extentElem.elementsByTagName( QStringLiteral( "value" ) );
  if ( valueNodeList.size() < 4 )
  {
    return QgsRectangle();
  }

  //order of value elements must be xmin, ymin, xmax, ymax
  double xmin = valueNodeList.at( 0 ).toElement().text().toDouble();
  double ymin = valueNodeList.at( 1 ).toElement().text().toDouble();
  double xmax = valueNodeList.at( 2 ).toElement().text().toDouble();
  double ymax = valueNodeList.at( 3 ).toElement().text().toDouble();
  return QgsRectangle( xmin, ymin, xmax, ymax );
}

QStringList QgsServerProjectParser::supportedOutputCrsList() const
{
  QStringList crsList;
  if ( !mXMLDoc )
  {
    return crsList;
  }

  QDomElement qgisElem = mXMLDoc->documentElement();
  if ( qgisElem.isNull() )
  {
    return crsList;
  }
  QDomElement propertiesElem = qgisElem.firstChildElement( QStringLiteral( "properties" ) );
  if ( propertiesElem.isNull() )
  {
    return crsList;
  }
  QDomElement wmsCrsElem = propertiesElem.firstChildElement( QStringLiteral( "WMSCrsList" ) );
  if ( !wmsCrsElem.isNull() )
  {
    QDomNodeList valueList = wmsCrsElem.elementsByTagName( QStringLiteral( "value" ) );
    for ( int i = 0; i < valueList.size(); ++i )
    {
      crsList.append( valueList.at( i ).toElement().text() );
    }
  }
  else
  {
    QDomElement wmsEpsgElem = propertiesElem.firstChildElement( QStringLiteral( "WMSEpsgList" ) );
    if ( !wmsEpsgElem.isNull() )
    {
      QDomNodeList valueList = wmsEpsgElem.elementsByTagName( QStringLiteral( "value" ) );
      bool conversionOk;
      for ( int i = 0; i < valueList.size(); ++i )
      {
        int epsgNr = valueList.at( i ).toElement().text().toInt( &conversionOk );
        if ( conversionOk )
        {
          crsList.append( QStringLiteral( "EPSG:%1" ).arg( epsgNr ) );
        }
      }
    }
    else
    {
      //no CRS restriction defined in the project. Provide project CRS, wgs84 and pseudo mercator
      QString projectCrsId = projectCrs().authid();
      crsList.append( projectCrsId );
      if ( projectCrsId.compare( QLatin1String( "EPSG:4326" ), Qt::CaseInsensitive ) != 0 )
      {
        crsList.append( QStringLiteral( "EPSG:%1" ).arg( 4326 ) );
      }
      if ( projectCrsId.compare( QLatin1String( "EPSG:3857" ), Qt::CaseInsensitive ) != 0 )
      {
        crsList.append( QStringLiteral( "EPSG:%1" ).arg( 3857 ) );
      }
    }
  }

  return crsList;
}

QString QgsServerProjectParser::projectTitle() const
{
  if ( !mXMLDoc )
  {
    return QString();
  }

  QDomElement qgisElem = mXMLDoc->documentElement();
  if ( qgisElem.isNull() )
  {
    return QString();
  }

  QDomElement titleElem = qgisElem.firstChildElement( QStringLiteral( "title" ) );
  if ( !titleElem.isNull() )
  {
    QString title = titleElem.text();
    if ( !title.isEmpty() )
    {
      return title;
    }
  }

  //no title element or not project title set. Use project filename without extension
  QFileInfo projectFileInfo( mProjectPath );
  return projectFileInfo.baseName();
}

QDomElement QgsServerProjectParser::legendElem() const
{
  if ( !mXMLDoc )
  {
    return QDomElement();
  }
  return mXMLDoc->documentElement().firstChildElement( QStringLiteral( "legend" ) );
}

QDomElement QgsServerProjectParser::propertiesElem() const
{
  if ( !mXMLDoc )
  {
    return QDomElement();
  }

  return mXMLDoc->documentElement().firstChildElement( QStringLiteral( "properties" ) );
}

QSet<QString> QgsServerProjectParser::findRestrictedLayers() const
{
  // get name of restricted layers/groups in project
  QStringList restricted = QgsServerProjectUtils::wmsRestrictedLayers( *mProject );

  // extract restricted layers from excluded groups
  QStringList restrictedLayersNames;
  QgsLayerTree *root = mProject->layerTreeRoot();

  Q_FOREACH ( QString l, restricted )
  {
    QgsLayerTreeGroup *group = root->findGroup( l );
    if ( group )
    {
      QList<QgsLayerTreeLayer *> groupLayers = group->findLayers();
      Q_FOREACH ( QgsLayerTreeLayer *treeLayer, groupLayers )
      {
        restrictedLayersNames.append( treeLayer->name() );
      }
    }
    else
    {
      restrictedLayersNames.append( l );
    }
  }

  // build output with names, ids or short name according to the configuration
  QSet<QString> restrictedLayers;
  QList<QgsLayerTreeLayer *> layers = root->findLayers();
  Q_FOREACH ( QgsLayerTreeLayer *layer, layers )
  {
    if ( restrictedLayersNames.contains( layer->name() ) )
    {
      QString shortName = layer->layer()->shortName();
      if ( QgsServerProjectUtils::wmsUseLayerIds( *mProject ) )
      {
        restrictedLayers.insert( layer->layerId() );
      }
      else if ( ! shortName.isEmpty() )
      {
        restrictedLayers.insert( shortName );
      }
      else
      {
        restrictedLayers.insert( layer->name() );
      }
    }
  }

  return restrictedLayers;
}

void QgsServerProjectParser::layerFromLegendLayer( const QDomElement &legendLayerElem, QMap< int, QgsMapLayer *> &layers, bool useCache ) const
{
  QString id = legendLayerElem.firstChild().firstChild().toElement().attribute( QStringLiteral( "layerid" ) );
  int drawingOrder = updateLegendDrawingOrder() ? -1 : mCustomLayerOrder.indexOf( id );

  QHash< QString, QDomElement >::const_iterator layerIt = mProjectLayerElementsById.find( id );
  if ( layerIt != mProjectLayerElementsById.constEnd() )
  {
    QgsMapLayer *layer = createLayerFromElement( layerIt.value(), useCache );
    if ( layer )
    {
      layers.insertMulti( drawingOrder, layer );
    }
  }
}

QList<QDomElement> QgsServerProjectParser::findLegendGroupElements() const
{
  QList<QDomElement> LegendGroupElemList;
  QgsLayerTreeGroup *rootLayerTreeGroup = new QgsLayerTreeGroup;

  QDomElement layerTreeElem = mXMLDoc->documentElement().firstChildElement( QStringLiteral( "layer-tree-group" ) );
  if ( !layerTreeElem.isNull() )
  {
    // this is apparently only used to retrieve groups - layers do not need to be resolved
    rootLayerTreeGroup = QgsLayerTreeGroup::readXml( layerTreeElem );
  }

  QDomElement legendElement = mXMLDoc->documentElement().firstChildElement( QStringLiteral( "legend" ) );
  if ( !legendElement.isNull() && rootLayerTreeGroup )
  {
    LegendGroupElemList.append( setLegendGroupElementsWithLayerTree( rootLayerTreeGroup, legendElement ) );
  }

  if ( !legendElement.isNull() )
  {
    QDomNodeList groupNodeList = legendElement.elementsByTagName( QStringLiteral( "legendgroup" ) );
    for ( int i = 0; i < groupNodeList.size(); ++i )
    {
      LegendGroupElemList.push_back( groupNodeList.at( i ).toElement() );
    }
    return LegendGroupElemList;
  }
  return LegendGroupElemList;
}

QList<QDomElement> QgsServerProjectParser::setLegendGroupElementsWithLayerTree( QgsLayerTreeGroup *layerTreeGroup, const QDomElement &legendElement ) const
{
  QList<QDomElement> LegendGroupElemList;
  QList< QgsLayerTreeNode * > layerTreeGroupChildren = layerTreeGroup->children();
  QDomNodeList legendElementChildNodes = legendElement.childNodes();
  int g = 0; // index of the last child layer tree group
  for ( int i = 0; i < legendElementChildNodes.size(); ++i )
  {
    QDomNode legendElementNode = legendElementChildNodes.at( i );
    if ( !legendElementNode.isElement() )
      continue;
    QDomElement legendElement = legendElementNode.toElement();
    if ( legendElement.tagName() != QLatin1String( "legendgroup" ) )
      continue;
    for ( int j = g; j < i + 1; ++j )
    {
      QgsLayerTreeNode *layerTreeNode = layerTreeGroupChildren.at( j );
      if ( layerTreeNode->nodeType() != QgsLayerTreeNode::NodeGroup )
        continue;
      QgsLayerTreeGroup *layerTreeGroup = static_cast<QgsLayerTreeGroup *>( layerTreeNode );
      if ( layerTreeGroup->name() == legendElement.attribute( QStringLiteral( "name" ) ) )
      {
        g = j;
        QString shortName = layerTreeGroup->customProperty( QStringLiteral( "wmsShortName" ) ).toString();
        if ( !shortName.isEmpty() )
          legendElement.setAttribute( QStringLiteral( "shortName" ), shortName );
        QString title = layerTreeGroup->customProperty( QStringLiteral( "wmsTitle" ) ).toString();
        if ( !title.isEmpty() )
          legendElement.setAttribute( QStringLiteral( "title" ), title );
        LegendGroupElemList.append( setLegendGroupElementsWithLayerTree( layerTreeGroup, legendElement ) );
      }
    }
    LegendGroupElemList.push_back( legendElement );
  }
  return LegendGroupElemList;
}

void QgsServerProjectParser::sublayersOfEmbeddedGroup( const QString &projectFilePath, const QString &groupName, QSet<QString> &layerSet )
{
  QFile projectFile( projectFilePath );
  if ( !projectFile.open( QIODevice::ReadOnly ) )
  {
    return;
  }

  QDomDocument xmlDoc;
  if ( !xmlDoc.setContent( &projectFile ) )
  {
    return;
  }

  //go to legend node
  QDomElement legendElem = xmlDoc.documentElement().firstChildElement( QStringLiteral( "legend" ) );
  if ( legendElem.isNull() )
  {
    return;
  }

  //get group node list of embedded project
  QDomNodeList groupNodes = legendElem.elementsByTagName( QStringLiteral( "legendgroup" ) );
  QDomElement groupElem;
  for ( int i = 0; i < groupNodes.size(); ++i )
  {
    groupElem = groupNodes.at( i ).toElement();
    if ( groupElem.attribute( QStringLiteral( "name" ) ) == groupName )
    {
      //get all subgroups and sublayers and add to layerSet
      QDomElement subElem;
      QDomNodeList subGroupList = groupElem.elementsByTagName( QStringLiteral( "legendgroup" ) );
      for ( int j = 0; j < subGroupList.size(); ++j )
      {
        subElem = subGroupList.at( j ).toElement();
        layerSet.insert( subElem.attribute( QStringLiteral( "name" ) ) );
      }
      QDomNodeList subLayerList = groupElem.elementsByTagName( QStringLiteral( "legendlayer" ) );
      for ( int j = 0; j < subLayerList.size(); ++j )
      {
        subElem = subLayerList.at( j ).toElement();
        layerSet.insert( subElem.attribute( QStringLiteral( "name" ) ) );
      }
    }
  }
}

QStringList QgsServerProjectParser::wfsLayers() const
{
  return QgsServerProjectUtils::wfsLayerIds( *mProject );
}

QStringList QgsServerProjectParser::wfsLayerNames() const
{
  QStringList layerNameList;

  QMap<QString, QgsMapLayer *> layerMap;
  projectLayerMap( layerMap );

  QgsMapLayer *currentLayer = nullptr;
  QStringList wfsIdList = QgsServerProjectUtils::wfsLayerIds( *mProject );
  QStringList::const_iterator wfsIdIt = wfsIdList.constBegin();
  for ( ; wfsIdIt != wfsIdList.constEnd(); ++wfsIdIt )
  {
    QMap<QString, QgsMapLayer *>::const_iterator layerMapIt = layerMap.find( *wfsIdIt );
    if ( layerMapIt != layerMap.constEnd() )
    {
      currentLayer = layerMapIt.value();
      if ( currentLayer )
      {
        bool useLayerIds = QgsServerProjectUtils::wmsUseLayerIds( *mProject );
        layerNameList.append( useLayerIds ? currentLayer->id() : currentLayer->name() );
      }
    }
  }

  return layerNameList;
}

QStringList QgsServerProjectParser::wcsLayerNames() const
{
  QStringList layerNameList;

  QMap<QString, QgsMapLayer *> layerMap;
  projectLayerMap( layerMap );

  QgsMapLayer *currentLayer = nullptr;
  QStringList wcsIdList = wcsLayers();
  QStringList::const_iterator wcsIdIt = wcsIdList.constBegin();
  for ( ; wcsIdIt != wcsIdList.constEnd(); ++wcsIdIt )
  {
    QMap<QString, QgsMapLayer *>::const_iterator layerMapIt = layerMap.find( *wcsIdIt );
    if ( layerMapIt != layerMap.constEnd() )
    {
      currentLayer = layerMapIt.value();
      if ( currentLayer )
      {
        bool useLayerIds = QgsServerProjectUtils::wmsUseLayerIds( *mProject );
        layerNameList.append( useLayerIds ? currentLayer->id() : currentLayer->name() );
      }
    }
  }

  return layerNameList;
}

QDomElement QgsServerProjectParser::firstComposerLegendElement() const
{
  if ( !mXMLDoc )
  {
    return QDomElement();
  }

  QDomElement documentElem = mXMLDoc->documentElement();
  if ( documentElem.isNull() )
  {
    return QDomElement();
  }

  QDomElement composerElem = documentElem.firstChildElement( QStringLiteral( "Composer" ) );
  if ( composerElem.isNull() )
  {
    return QDomElement();
  }
  QDomElement compositionElem = composerElem.firstChildElement( QStringLiteral( "Composition" ) );
  if ( compositionElem.isNull() )
  {
    return QDomElement();
  }
  return compositionElem.firstChildElement( QStringLiteral( "ComposerLegend" ) );
}

QList<QDomElement> QgsServerProjectParser::publishedComposerElements() const
{
  QList<QDomElement> composerElemList;
  if ( !mXMLDoc )
  {
    return composerElemList;
  }

  QDomNodeList composerNodeList = mXMLDoc->elementsByTagName( QStringLiteral( "Composer" ) );

  QDomElement propertiesElem = mXMLDoc->documentElement().firstChildElement( QStringLiteral( "properties" ) );
  QDomElement wmsRestrictedComposersElem = propertiesElem.firstChildElement( QStringLiteral( "WMSRestrictedComposers" ) );
  if ( wmsRestrictedComposersElem.isNull() )
  {
    for ( int i = 0; i < composerNodeList.size(); ++i )
    {
      composerElemList.push_back( composerNodeList.at( i ).toElement() );
    }
    return composerElemList;
  }

  QSet<QString> restrictedComposerNames;
  QDomNodeList valueList = wmsRestrictedComposersElem.elementsByTagName( QStringLiteral( "value" ) );
  for ( int i = 0; i < valueList.size(); ++i )
  {
    restrictedComposerNames.insert( valueList.at( i ).toElement().text() );
  }

  //remove unpublished composers from list
  QString currentComposerName;
  QDomElement currentElem;
  for ( int i = 0; i < composerNodeList.size(); ++i )
  {
    currentElem = composerNodeList.at( i ).toElement();
    currentComposerName = currentElem.attribute( QStringLiteral( "title" ) );
    if ( !restrictedComposerNames.contains( currentComposerName ) )
    {
      composerElemList.push_back( currentElem );
    }
  }

  return composerElemList;
}

QList< QPair< QString, QgsDatumTransformStore::Entry > > QgsServerProjectParser::layerCoordinateTransforms() const
{
  QList< QPair< QString, QgsDatumTransformStore::Entry > > layerTransformList;

  QDomElement coordTransformInfoElem = mXMLDoc->documentElement().firstChildElement( QStringLiteral( "mapcanvas" ) ).firstChildElement( QStringLiteral( "layer_coordinate_transform_info" ) );
  if ( coordTransformInfoElem.isNull() )
  {
    return layerTransformList;
  }

  QDomNodeList layerTransformNodeList = coordTransformInfoElem.elementsByTagName( QStringLiteral( "layer_coordinate_transform" ) );
  layerTransformList.reserve( layerTransformNodeList.size() );
  for ( int i = 0; i < layerTransformNodeList.size(); ++i )
  {
    QPair< QString, QgsDatumTransformStore::Entry > layerEntry;
    QDomElement layerTransformElem = layerTransformNodeList.at( i ).toElement();
    layerEntry.first = layerTransformElem.attribute( QStringLiteral( "layerid" ) );
    QgsDatumTransformStore::Entry t;
    t.srcAuthId = layerTransformElem.attribute( QStringLiteral( "srcAuthId" ) );
    t.destAuthId = layerTransformElem.attribute( QStringLiteral( "destAuthId" ) );
    t.srcDatumTransform = layerTransformElem.attribute( QStringLiteral( "srcDatumTransform" ), QStringLiteral( "-1" ) ).toInt();
    t.destDatumTransform = layerTransformElem.attribute( QStringLiteral( "destDatumTransform" ), QStringLiteral( "-1" ) ).toInt();
    layerEntry.second = t;
    layerTransformList.push_back( layerEntry );
  }
  return layerTransformList;
}

QStringList QgsServerProjectParser::wcsLayers() const
{
  return QgsServerProjectUtils::wcsLayerIds( *mProject );
}

void QgsServerProjectParser::addJoinLayersForElement( const QDomElement &layerElem ) const
{
  QDomElement vectorJoinsElem = layerElem.firstChildElement( QStringLiteral( "vectorjoins" ) );
  if ( vectorJoinsElem.isNull() )
  {
    return;
  }

  QDomNodeList joinNodeList = vectorJoinsElem.elementsByTagName( QStringLiteral( "join" ) );
  for ( int i = 0; i < joinNodeList.size(); ++i )
  {
    QString id = joinNodeList.at( i ).toElement().attribute( QStringLiteral( "joinLayerId" ) );
    QgsMapLayer *layer = mapLayerFromLayerId( id );
    if ( layer && !QgsProject::instance()->mapLayer( id ) )
    {
      QgsProject::instance()->addMapLayer( layer, false, false );
    }
  }
}

void QgsServerProjectParser::addValueRelationLayersForLayer( const QgsVectorLayer *vl ) const
{
  if ( !vl )
    return;

  for ( int idx = 0; idx < vl->pendingFields().size(); idx++ )
  {
    const QString name = vl->pendingFields().field( idx ).name();
    if ( vl->editorWidgetSetup( idx ).type() != QLatin1String( "ValueRelation" ) )
      continue;

    QVariantMap cfg( vl->editorWidgetSetup( idx ).config() );
    if ( !cfg.contains( QStringLiteral( "Layer" ) ) )
      continue;

    QString layerId = cfg.value( QStringLiteral( "Layer" ) ).toString();
    if ( QgsProject::instance()->mapLayer( layerId ) )
      continue;

    QgsMapLayer *layer = mapLayerFromLayerId( layerId );
    if ( !layer )
      continue;

    QgsProject::instance()->addMapLayer( layer, false, false );
  }
}

void QgsServerProjectParser::addGetFeatureLayers( const QDomElement &layerElem ) const
{
  QString str;
  QTextStream stream( &str );
  layerElem.save( stream, 2 );

  QRegExp rx( "getFeature\\('([^']*)'" );
  int idx = 0;
  while ( ( idx = rx.indexIn( str, idx ) ) != -1 )
  {
    QString name = rx.cap( 1 );
    QgsMapLayer *ml = nullptr;
    QHash< QString, QDomElement >::const_iterator layerElemIt = mProjectLayerElementsById.find( name );
    if ( layerElemIt != mProjectLayerElementsById.constEnd() )
    {
      ml = createLayerFromElement( layerElemIt.value() );
    }
    else
    {
      layerElemIt = mProjectLayerElementsByName.find( name );
      if ( layerElemIt != mProjectLayerElementsByName.constEnd() )
      {
        ml = createLayerFromElement( layerElemIt.value() );
      }
    }

    if ( ml )
    {
      QgsProject::instance()->addMapLayer( ml, false, false );
    }
    idx += rx.matchedLength();
  }
}


