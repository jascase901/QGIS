/************************************************************************
 * This file has been generated automatically from                      *
 *                                                                      *
 * src/core/processing/models/qgsprocessingmodelparameter.h             *
 *                                                                      *
 * Do not edit manually ! Edit header and run scripts/sipify.pl again   *
 ************************************************************************/






class QgsProcessingModelParameter : QgsProcessingModelComponent
{
%Docstring
 Represents an input parameter used by the model.
.. versionadded:: 3.0
%End

%TypeHeaderCode
#include "qgsprocessingmodelparameter.h"
%End
  public:

    QgsProcessingModelParameter( const QString &parameterName = QString() );
%Docstring
 Constructor for QgsProcessingModelParameter. The parameter name should match one of the
 parameters from the parent model.
%End

    QString parameterName() const;
%Docstring
 Returns the associated parameter name. The parameter name should match one of the
 parameters from the parent model.
.. seealso:: parameterName()
 :rtype: str
%End

    void setParameterName( const QString &name );
%Docstring
 Sets the associated parameter name. The parameter name should match one of the
 parameters from the parent model.
.. seealso:: parameterName()
%End

    QVariant toVariant() const;
%Docstring
 Saves this parameter to a QVariant.
.. seealso:: loadVariant()
 :rtype: QVariant
%End

    bool loadVariant( const QVariantMap &map );
%Docstring
 Loads this parameter from a QVariantMap.
.. seealso:: toVariant()
 :rtype: bool
%End

};


/************************************************************************
 * This file has been generated automatically from                      *
 *                                                                      *
 * src/core/processing/models/qgsprocessingmodelparameter.h             *
 *                                                                      *
 * Do not edit manually ! Edit header and run scripts/sipify.pl again   *
 ************************************************************************/
