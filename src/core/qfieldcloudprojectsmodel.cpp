/***************************************************************************
    qfieldcloudprojectsmodel.cpp
    ---------------------
    begin                : January 2020
    copyright            : (C) 2020 by Matthias Kuhn
    email                : matthias at opengis dot ch
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "deltafilewrapper.h"
#include "fileutils.h"
#include "layerobserver.h"
#include "qfield.h"
#include "qfieldcloudconnection.h"
#include "qfieldcloudprojectsmodel.h"
#include "qfieldcloudutils.h"

#include <QDir>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QSettings>
#include <QTemporaryFile>
#include <qgis.h>
#include <qgsapplication.h>
#include <qgsmessagelog.h>
#include <qgsnetworkaccessmanager.h>
#include <qgsproject.h>
#include <qgsproviderregistry.h>

#define MAX_REDIRECTS_ALLOWED 10
#define MAX_PARALLEL_REQUESTS 6
#define CACHE_PROJECT_DATA_SECS 5

QFieldCloudProjectsModel::QFieldCloudProjectsModel()
  : mProject( QgsProject::instance() )
{
  QJsonArray projects;
  reload( projects );

  // TODO all of these connects are a bit too much, and I guess not very precise, should be refactored!
  connect( this, &QFieldCloudProjectsModel::currentProjectIdChanged, this, [=]() {
    CloudProject *project = findProject( mCurrentProjectId );

    if ( !project )
      return;

    refreshProjectModification( mCurrentProjectId );
  } );

  connect( this, &QFieldCloudProjectsModel::modelReset, this, [=]() {
    if ( mCurrentProjectId.isEmpty() || !findProject( mCurrentProjectId ) )
      return;

    emit currentProjectDataChanged();

    refreshProjectModification( mCurrentProjectId );
  } );

  connect( this, &QFieldCloudProjectsModel::dataChanged, this, [=]( const QModelIndex &topLeft, const QModelIndex &bottomRight, const QVector<int> &roles ) {
    Q_UNUSED( bottomRight )
    Q_UNUSED( roles )

    if ( mCurrentProjectId.isEmpty() )
      return;

    const QModelIndex projectIndex = findProjectIndex( mCurrentProjectId );

    if ( !projectIndex.isValid() )
      return;

    // current project
    if ( topLeft.row() == projectIndex.row() )
    {
      emit currentProjectDataChanged();
    }
  } );

  connect( this, &QFieldCloudProjectsModel::cloudConnectionChanged, this, [=]() {
    if ( !mCloudConnection )
      return;

    mUsername = mCloudConnection->username();
    if ( mCloudConnection->status() != QFieldCloudConnection::ConnectionStatus::LoggedIn )
    {
      QJsonArray projects;
      reload( projects );
    }
    connect( mCloudConnection, &QFieldCloudConnection::usernameChanged, this, [=]() {
      mUsername = mCloudConnection->username();
    } );
  } );
}

QFieldCloudConnection *QFieldCloudProjectsModel::cloudConnection() const
{
  return mCloudConnection;
}

void QFieldCloudProjectsModel::setCloudConnection( QFieldCloudConnection *cloudConnection )
{
  if ( mCloudConnection == cloudConnection )
    return;

  if ( cloudConnection )
    connect( cloudConnection, &QFieldCloudConnection::statusChanged, this, &QFieldCloudProjectsModel::connectionStatusChanged );

  mCloudConnection = cloudConnection;
  emit cloudConnectionChanged();
}

LayerObserver *QFieldCloudProjectsModel::layerObserver() const
{
  return mLayerObserver;
}

void QFieldCloudProjectsModel::setLayerObserver( LayerObserver *layerObserver )
{
  if ( mLayerObserver == layerObserver )
    return;

  mLayerObserver = layerObserver;

  if ( !layerObserver )
    return;

  connect( layerObserver, &LayerObserver::layerEdited, this, &QFieldCloudProjectsModel::layerObserverLayerEdited );
  connect( layerObserver->deltaFileWrapper(), &DeltaFileWrapper::countChanged, this, [=]() {
    refreshProjectModification( mCurrentProjectId );
  } );

  emit layerObserverChanged();
}

QString QFieldCloudProjectsModel::currentProjectId() const
{
  return mCurrentProjectId;
}

void QFieldCloudProjectsModel::setCurrentProjectId( const QString &currentProjectId )
{
  if ( mCurrentProjectId == currentProjectId )
    return;

  mCurrentProjectId = currentProjectId;

  emit currentProjectIdChanged();
  emit currentProjectDataChanged();
}

QVariantMap QFieldCloudProjectsModel::currentProjectData() const
{
  if ( mCurrentProjectId.isEmpty() )
  {
    return QVariantMap();
  }

  return getProjectData( mCurrentProjectId );
}

QVariantMap QFieldCloudProjectsModel::getProjectData( const QString &projectId ) const
{
  QVariantMap data;

  const QModelIndex projectIndex = findProjectIndex( projectId );

  if ( !projectIndex.isValid() )
    return data;

  const QHash<int, QByteArray> rn = this->roleNames();

  for ( auto [key, value] : qfield::asKeyValueRange( rn ) )
  {
    data[value] = projectIndex.data( key );
  }

  return data;
}

void QFieldCloudProjectsModel::refreshProjectsList()
{
  switch ( mCloudConnection->status() )
  {
    case QFieldCloudConnection::ConnectionStatus::LoggedIn:
    {
      NetworkReply *reply = mCloudConnection->get( QStringLiteral( "/api/v1/projects/?include-public=true" ) );
      connect( reply, &NetworkReply::finished, this, &QFieldCloudProjectsModel::projectListReceived );
      break;
    }
    case QFieldCloudConnection::ConnectionStatus::Disconnected:
    {
      // Nothing done at the moment; since the connection can be disconnected after discovering
      // a token has been invalidated, be careful with what is added here.
      break;
    }
    case QFieldCloudConnection::ConnectionStatus::Connecting:
      // Nothing done for this intermediary status.
      break;
  }
}

QModelIndex QFieldCloudProjectsModel::findProjectIndex( const QString &projectId ) const
{
  if ( projectId.isEmpty() )
  {
    QgsLogger::debug( QStringLiteral( "No project found for an empty project id." ) );
    return QModelIndex();
  }

  for ( int i = 0; i < mProjects.count(); i++ )
  {
    if ( mProjects.at( i )->id == projectId )
    {
      return createIndex( i, 0 );
    }
  }

  QgsLogger::debug( QStringLiteral( "No project found with the provided id: `%1`" ).arg( projectId ) );

  return QModelIndex();
}

QFieldCloudProjectsModel::CloudProject *QFieldCloudProjectsModel::findProject( const QString &projectId ) const
{
  const QModelIndex index = findProjectIndex( projectId );

  if ( index.isValid() )
    return mProjects[index.row()];

  return nullptr;
}

void QFieldCloudProjectsModel::removeLocalProject( const QString &projectId )
{
  QDir dir( QStringLiteral( "%1/%2/%3" ).arg( QFieldCloudUtils::localCloudDirectory(), mUsername, projectId ) );

  if ( dir.exists() )
  {
    const QModelIndex projectIndex = findProjectIndex( projectId );

    if ( projectIndex.isValid() )
    {
      CloudProject *project = mProjects[projectIndex.row()];
      if ( project->status == ProjectStatus::Idle && project->checkout & RemoteCheckout )
      {
        project->localPath = QString();
        project->checkout = RemoteCheckout;
        project->modification = NoModification;
        emit dataChanged( projectIndex, projectIndex, QVector<int>() << StatusRole << LocalPathRole << CheckoutRole );
      }
      else
      {
        beginRemoveRows( QModelIndex(), projectIndex.row(), projectIndex.row() );
        mProjects.removeAt( projectIndex.row() );
        endRemoveRows();
      }
    }

    dir.removeRecursively();
  }

  QSettings().remove( QStringLiteral( "QFieldCloud/projects/%1" ).arg( projectId ) );
}

QFieldCloudProjectsModel::ProjectStatus QFieldCloudProjectsModel::projectStatus( const QString &projectId ) const
{
  CloudProject *project = findProject( projectId );

  if ( !project )
    return QFieldCloudProjectsModel::ProjectStatus::Idle;

  return project->status;
}

bool QFieldCloudProjectsModel::canSyncProject( const QString &projectId ) const
{
  CloudProject *project = findProject( projectId );

  if ( !project )
    return false;

  if ( mCurrentProjectId.isEmpty() )
    return false;
  else if ( projectStatus( projectId ) == ProjectStatus::Idle )
    return true;

  return false;
}

QFieldCloudProjectsModel::ProjectModifications QFieldCloudProjectsModel::projectModification( const QString &projectId ) const
{
  CloudProject *project = findProject( projectId );

  if ( !project )
    return NoModification;

  return project->modification;
}

void QFieldCloudProjectsModel::refreshProjectModification( const QString &projectId )
{
  const QModelIndex projectIndex = findProjectIndex( projectId );
  if ( !projectIndex.isValid() )
    return;

  CloudProject *project = mProjects[projectIndex.row()];
  ProjectModifications oldModifications = project->modification;

  if ( mLayerObserver->deltaFileWrapper()->count() > 0 )
  {
    project->modification |= LocalModification;
  }
  else if ( project->modification & LocalModification )
  {
    project->modification ^= LocalModification;
  }

  if ( oldModifications != project->modification )
  {
    emit dataChanged( projectIndex, projectIndex, QVector<int>() << ModificationRole << CanSyncRole );
  }
}

QString QFieldCloudProjectsModel::layerFileName( const QgsMapLayer *layer ) const
{
  return layer->dataProvider()->dataSourceUri().split( '|' )[0];
}

QFieldCloudProjectsModel::JobStatus QFieldCloudProjectsModel::jobStatus( const QString &status ) const
{
  const QString statusLower = status.toLower();
  if ( statusLower == QStringLiteral( "pending" ) )
    return JobPendingStatus;
  else if ( statusLower == QStringLiteral( "queued" ) )
    return JobQueuedStatus;
  else if ( statusLower == QStringLiteral( "started" ) )
    return JobStartedStatus;
  else if ( statusLower == QStringLiteral( "stopped" ) )
    return JobStoppedStatus;
  else if ( statusLower == QStringLiteral( "finished" ) )
    return JobFinishedStatus;
  //cppcheck-suppress duplicateBranch
  else if ( statusLower == QStringLiteral( "failed" ) )
    return JobFailedStatus;
  else
    // "STATUS_ERROR" or any unknown status is considered an error
    return JobFailedStatus;
}

void QFieldCloudProjectsModel::projectCancelDownload( const QString &projectId )
{
  if ( !mCloudConnection )
    return;

  const QModelIndex projectIndex = findProjectIndex( projectId );
  if ( !projectIndex.isValid() )
    return;

  CloudProject *project = mProjects[projectIndex.row()];

  // before canceling, the project should be downloading
  if ( project->status != ProjectStatus::Downloading )
    return;

  project->packagingStatus = PackagingAbortStatus;

  const QStringList fileNames = project->downloadFileTransfers.keys();
  for ( const QString &fileName : fileNames )
  {
    NetworkReply *reply = project->downloadFileTransfers[fileName].networkReply;

    if ( reply )
      reply->abort();

    project->downloadFileTransfers.remove( fileName );
  }

  QgsMessageLog::logMessage( QStringLiteral( "Download of project id `%1` aborted" ).arg( projectId ) );

  project->errorStatus = NoErrorStatus;
  project->isPackagingActive = false;
  project->isPackagingFailed = true;
  project->isPackagingAborted = true;
  project->packagingStatusString = tr( "aborted" );
  project->status = ProjectStatus::Idle;

  emit dataChanged( projectIndex, projectIndex, QVector<int>() << StatusRole << ErrorStatusRole << PackagingErrorStatus );
}

void QFieldCloudProjectsModel::projectRefreshData( const QString &projectId, const ProjectRefreshReason &refreshReason )
{
  if ( !mCloudConnection )
    return;

  const QModelIndex projectIndex = findProjectIndex( projectId );
  if ( !projectIndex.isValid() )
    return;

  CloudProject *project = mProjects[projectIndex.row()];

  NetworkReply *reply = mCloudConnection->get( QStringLiteral( "/api/v1/projects/%1/" ).arg( projectId ) );
  connect( reply, &NetworkReply::finished, reply, [=]() {
    if ( !findProject( projectId ) )
      return;

    QNetworkReply *rawReply = reply->reply();

    reply->deleteLater();

    if ( rawReply->error() != QNetworkReply::NoError )
    {
      emit projectRefreshed( projectId, refreshReason, QFieldCloudConnection::errorString( rawReply ) );
      return;
    }

    const QJsonObject projectData = QJsonDocument::fromJson( rawReply->readAll() ).object();
    const QString projectId = projectData.value( "id" ).toString();

    if ( projectData.value( "id" ).toString() != projectId )
      return;

    QgsLogger::debug( QStringLiteral( "Project %1: data refreshed." ).arg( projectId ) );

    if (
      !projectData.value( "name" ).isString()
      || !projectData.value( "owner" ).isString()
      || !projectData.value( "description" ).isString()
      || !projectData.value( "user_role" ).isString()
      || !projectData.value( "is_public" ).isBool()
      || !projectData.value( "can_repackage" ).isBool()
      || !projectData.value( "needs_repackaging" ).isBool() )
    {
      emit projectRefreshed( projectId, refreshReason, tr( "project(%1) trigger response refresh not contain all the expected keys: name(string), owner(string), description(string), user_role(string), is_public(bool), can_repackage(bool), needs_repackaging(bool)" ).arg( projectId ) );
      return;
    }

    project->name = projectData.value( "name" ).toString();
    project->owner = projectData.value( "owner" ).toString();
    project->description = projectData.value( "description" ).toString();
    project->userRole = projectData.value( "user_role" ).toString();
    project->isPrivate = projectData.value( "is_public" ).isUndefined() ? projectData.value( "private" ).toBool() : !projectData.value( "is_public" ).toBool( false );
    project->canRepackage = projectData.value( "can_repackage" ).toBool();
    project->needsRepackaging = projectData.value( "needs_repackaging" ).toBool();
    project->lastRefreshedAt = QDateTime::currentDateTimeUtc();

    QFieldCloudUtils::setProjectSetting( project->id, QStringLiteral( "name" ), project->name );
    QFieldCloudUtils::setProjectSetting( project->id, QStringLiteral( "owner" ), project->owner );
    QFieldCloudUtils::setProjectSetting( project->id, QStringLiteral( "description" ), project->description );
    QFieldCloudUtils::setProjectSetting( project->id, QStringLiteral( "userRole" ), project->userRole );
    QFieldCloudUtils::setProjectSetting( project->id, QStringLiteral( "updatedAt" ), project->updatedAt );
    QFieldCloudUtils::setProjectSetting( project->id, QStringLiteral( "isPrivate" ), project->isPrivate );
    QFieldCloudUtils::setProjectSetting( project->id, QStringLiteral( "canRepackage" ), project->canRepackage );
    QFieldCloudUtils::setProjectSetting( project->id, QStringLiteral( "needsRepackaging" ), project->needsRepackaging );

    emit dataChanged( projectIndex, projectIndex, QVector<int>::fromList( roleNames().keys() ) );
    emit projectRefreshed( projectId, refreshReason );
  } );
}

void QFieldCloudProjectsModel::projectStartJob( const QString &projectId, const JobType jobType )
{
  const QModelIndex projectIndex = findProjectIndex( projectId );
  if ( !projectIndex.isValid() )
    return;

  CloudProject *project = mProjects[projectIndex.row()];
  project->jobs[jobType] = Job( QString(), projectId, jobType );

  emit dataChanged( projectIndex, projectIndex, QVector<int>::fromList( roleNames().keys() ) );

  auto getStringJobType = []( JobType jobType ) {
    switch ( jobType )
    {
      case JobType::Package:
        return QStringLiteral( "package" );
    }

    return QString();
  };

  QString jobTypeName = getStringJobType( jobType );
  QgsLogger::debug( QStringLiteral( "Project %1: creating a new `%2` job..." ).arg( projectId, jobTypeName ) );
  NetworkReply *reply = mCloudConnection->post(
    QStringLiteral( "/api/v1/jobs/" ),
    QVariantMap(
      {
        { "project_id", projectId },
        { "type", jobTypeName },
      } ) );

  connect( reply, &NetworkReply::finished, reply, [=]() {
    reply->deleteLater();

    if ( project->isPackagingAborted )
    {
      QgsLogger::debug( QStringLiteral( "Project %1: job creation finished, but project operations are aborted" ).arg( projectId ) );
      return;
    }

    // the project has been deleted
    if ( !findProject( projectId ) )
    {
      QgsLogger::debug( QStringLiteral( "Project %1: job creation finished, but the project is deleted." ).arg( projectId ) );
      return;
    }

    QNetworkReply *rawReply = reply->reply();

    if ( rawReply->error() != QNetworkReply::NoError )
    {
      QString errorString = QFieldCloudConnection::errorString( rawReply );
      QgsLogger::debug( QStringLiteral( "Project %1: job creation failed with an error: %2" ).arg( projectId, errorString ) );
      emit projectJobFinished( projectId, jobType, errorString );
      return;
    }

    const QJsonObject payload = QJsonDocument::fromJson( rawReply->readAll() ).object();
    const QString jobId = payload.value( QStringLiteral( "id" ) ).toString();

    if ( jobId.isEmpty() )
    {
      QgsLogger::debug( QStringLiteral( "Project %1: job creation finished, but missing id key from the response payload" ).arg( projectId ) );
      emit projectJobFinished( projectId, jobType, tr( "Job creation finished, but the server response is missing required fields: id(string)" ).arg( jobId ) );
      return;
    }

    QgsLogger::debug( QStringLiteral( "Project %1: created job with id `%2`" ).arg( projectId ).arg( jobId ) );

    project->jobs[jobType].id = jobId;

    emit dataChanged( projectIndex, projectIndex, QVector<int>() << PackagingStatusRole );

    projectGetJobStatus( projectId, jobType );
  } );
}


void QFieldCloudProjectsModel::projectGetJobStatus( const QString &projectId, const JobType jobType )
{
  CloudProject *project = findProject( projectId );

  if ( !project )
  {
    QgsLogger::debug( QStringLiteral( "Project %1: getting job status, but the project is deleted." ).arg( projectId ) );
    return;
  }

  if ( project->isPackagingAborted )
  {
    QgsLogger::debug( QStringLiteral( "Project %1: getting job status, but project operations are aborted." ).arg( projectId ) );
    return;
  }

  if ( !project->jobs.contains( jobType ) )
  {
    QgsLogger::debug( QStringLiteral( "Project %1: getting job status, but no package job triggered yet." ).arg( projectId ) );
    return;
  }

  QString jobId = project->jobs[jobType].id;
  QgsLogger::debug( QStringLiteral( "Project %1, job %2: getting job status..." ).arg( projectId, jobId ) );
  NetworkReply *reply = mCloudConnection->get( QStringLiteral( "/api/v1/jobs/%1/" ).arg( jobId ) );

  connect( reply, &NetworkReply::finished, this, [=]() {
    reply->deleteLater();

    // the project has been deleted
    if ( !findProject( projectId ) )
    {
      QgsLogger::debug( QStringLiteral( "Project %1, job %2: getting job status finished, but the project is deleted." ).arg( projectId, jobId ) );
      return;
    }

    if ( project->isPackagingAborted )
    {
      QgsLogger::debug( QStringLiteral( "Project %1, job %2: getting job status finished, but project operations are aborted." ).arg( projectId, jobId ) );
      return;
    }

    QNetworkReply *rawReply = reply->reply();

    if ( rawReply->error() != QNetworkReply::NoError )
    {
      QString errorString = QFieldCloudConnection::errorString( rawReply );
      QgsLogger::debug( QStringLiteral( "Project %1, job %2: getting job status finished with an error: %3." ).arg( projectId, jobId, errorString ) );
      project->jobs[jobType].status = JobFailedStatus;
      emit projectJobFinished( projectId, jobType, errorString );
      return;
    }

    const QJsonObject payload = QJsonDocument::fromJson( rawReply->readAll() ).object();
    const QString jobStatusString = payload.value( QStringLiteral( "status" ) ).toString();

    if ( jobStatusString.isEmpty() )
    {
      QgsLogger::debug( QStringLiteral( "Project %1, job %2: getting job status finished, but missing status key from the response payload" ).arg( projectId, jobId ) );

      project->jobs[jobType].status = JobFailedStatus;
      emit projectJobFinished( projectId, jobType, tr( "job(%1) status response does not contain all the expected keys: status(string)" ).arg( jobId ) );
      return;
    }

    project->jobs[jobType].status = jobStatus( jobStatusString );

    QgsLogger::debug( QStringLiteral( "Project %1, job %2: getting job status finished with `%3`" ).arg( projectId, jobId, jobStatusString ) );

    switch ( project->jobs[jobType].status )
    {
      case JobPendingStatus:
      case JobQueuedStatus:
      case JobStartedStatus:
      case JobStoppedStatus:
        // infinite retry, there should be one day, when we can get the status!
        QTimer::singleShot( sDelayBeforeStatusRetry, [=]() {
          projectGetJobStatus( projectId, jobType );
        } );
        break;

      case JobFailedStatus:
        emit projectJobFinished( projectId, jobType, tr( "Job(%1) finished with a failed status." ).arg( jobId ) );
        return;
      case JobFinishedStatus:
        emit projectJobFinished( projectId, jobType );
        return;
    }
  } );
}

void QFieldCloudProjectsModel::projectPackageAndDownload( const QString &projectId )
{
  QgsLogger::debug( QStringLiteral( "Project %1: package and download initiated" ).arg( projectId ) );

  if ( !mCloudConnection )
    return;

  const QModelIndex projectIndex = findProjectIndex( projectId );
  if ( !projectIndex.isValid() )
    return;

  CloudProject *project = mProjects[projectIndex.row()];

  if ( project->status != ProjectStatus::Idle )
  {
    QgsLogger::debug( QStringLiteral( "Project %1: package and download cancelled, the project is currently busy" ).arg( projectId ) );
    emit warning( tr( "Project busy." ) );
    return;
  }

  project->packagingStatus = PackagingUnstartedStatus;
  project->packagingStatusString = QString();
  project->packagedLayerErrors.clear();
  project->downloadFileTransfers.clear();
  project->downloadFilesFinished = 0;
  project->downloadFilesFailed = 0;
  project->downloadBytesTotal = 0;
  project->downloadBytesReceived = 0;
  project->downloadProgress = 0;
  project->status = ProjectStatus::Downloading;
  project->errorStatus = NoErrorStatus;
  project->modification = NoModification;

  emit dataChanged( projectIndex, projectIndex );

  auto projectRepackageIfNeededAndThenDownload = [=]() {
    if ( project->needsRepackaging )
    {
      QgsLogger::debug( QStringLiteral( "Project %1: repackaging triggered." ).arg( projectId ) );

      projectStartJob( projectId, JobType::Package );

      QObject *tempProjectJobFinishedParent = new QObject( this ); // we need this to unsubscribe
      connect( this, &QFieldCloudProjectsModel::projectJobFinished, tempProjectJobFinishedParent, [=]( const QString &jobProjectId, const JobType jobType, const QString &errorString ) {
        if ( jobProjectId != projectId )
        {
          QgsLogger::debug( QStringLiteral( "Project %1: packaging finished for unexpected project `%2`." ).arg( projectId, jobProjectId ) );
          return;
        }

        if ( jobType != JobType::Package )
        {
          QgsLogger::debug( QStringLiteral( "Project %1: unexpected job type, expected %2 but %3 received." ).arg( projectId, static_cast<int>( JobType::Package ), static_cast<int>( jobType ) ) );
          Q_ASSERT( 0 );
          return;
        }

        tempProjectJobFinishedParent->deleteLater();

        // the project has been deleted
        if ( !findProject( projectId ) )
        {
          QgsLogger::debug( QStringLiteral( "Project %1: packaging finished, but the project is deleted." ).arg( projectId ) );
          return;
        }

        if ( project->packagingStatus == PackagingAbortStatus )
        {
          QgsLogger::debug( QStringLiteral( "Project %1: packaging finished, but project operations are aborted." ).arg( projectId ) );
          return;
        }

        if ( project->jobs[jobType].status != JobFinishedStatus )
        {
          QgsLogger::warning( QStringLiteral( "Project %1: packaging has an error: %2" ).arg( projectId, errorString ) );

          project->jobs.take( jobType );

          emit projectDownloadFinished( projectId, tr( "Packaging job finished unsuccessfully for `%1`. %2" )
                                                     .arg( project->name )
                                                     .arg( errorString ) );
          return;
        }

        projectDownload( projectId );
      } );
    }
    else
    {
      projectDownload( projectId );
    }
  };

  // Check and refresh project data if needed, because it might be outdated
  if ( !project->lastRefreshedAt.isValid()
       || project->lastRefreshedAt.secsTo( QDateTime::currentDateTimeUtc() ) > CACHE_PROJECT_DATA_SECS )
  {
    QgsLogger::debug( QStringLiteral( "Project %1: refreshing data..." ).arg( projectId ) );

    projectRefreshData( projectId, ProjectRefreshReason::Package );

    QObject *tempProjectRefreshParent = new QObject( this ); // we need this to unsubscribe
    connect( this, &QFieldCloudProjectsModel::projectRefreshed, tempProjectRefreshParent, [=]( const QString &refreshedProjectId, const ProjectRefreshReason refreshReason, const QString &errorString ) {
      if ( refreshedProjectId != projectId )
      {
        QgsLogger::critical( QStringLiteral( "Project %1: refereshing data finished for unexpected project `%2`." ).arg( projectId, refreshedProjectId ) );
        Q_ASSERT( 0 );
        return;
      }

      if ( refreshReason != ProjectRefreshReason::Package )
      {
        QgsLogger::critical( QStringLiteral( "Project %1: unexpected job type, expected %2 but %3 received." ).arg( projectId ).arg( static_cast<int>( ProjectRefreshReason::Package ), static_cast<int>( refreshReason ) ) );
        Q_ASSERT( 0 );
        return;
      }

      tempProjectRefreshParent->deleteLater();

      // the project has been deleted
      if ( !findProject( projectId ) )
      {
        QgsLogger::debug( QStringLiteral( "Project %1: refreshing data finished, but the project is deleted." ).arg( projectId ) );
        return;
      }

      if ( project->packagingStatus == PackagingAbortStatus )
      {
        QgsLogger::debug( QStringLiteral( "Project %1: refreshing data finished, but project operations are aborted." ).arg( projectId ) );
        return;
      }

      if ( !errorString.isNull() )
      {
        QgsLogger::debug( QStringLiteral( "Project %1: refreshing data finished with an error: %2." ).arg( projectId, errorString ) );
        emit projectDownloadFinished( projectId, tr( "Failed to refresh the latest info for `%1`: %2" ).arg( project->name, errorString ) );
        return;
      }

      projectRepackageIfNeededAndThenDownload();
    } );
  }
  else
  {
    projectRepackageIfNeededAndThenDownload();
  }

  QObject *tempProjectDownloadFinishedParent = new QObject( this ); // we need this to unsubscribe
  connect( this, &QFieldCloudProjectsModel::projectDownloadFinished, tempProjectDownloadFinishedParent, [=]( const QString &finishedProjectId, const QString &errorString ) {
    if ( finishedProjectId != projectId )
      return;

    if ( project->isPackagingAborted )
      return;

    tempProjectDownloadFinishedParent->deleteLater();

    // the project has been deleted
    if ( !findProject( projectId ) )
      return;

    if ( project->packagingStatus == PackagingAbortStatus )
      return;

    mActiveProjectFilesToDownload.clear();

    const bool hasError = !errorString.isNull();

    if ( hasError )
    {
      project->errorStatus = DownloadErrorStatus;
      project->packagingStatus = PackagingErrorStatus;
      project->packagingStatusString = errorString;

      QgsMessageLog::logMessage( QStringLiteral( "Downloading project `%1` finished with an error: %2" ).arg( projectId ).arg( project->packagingStatusString ) );
    }
    else
    {
      project->errorStatus = NoErrorStatus;
      project->packagingStatus = PackagingFinishedStatus;
      project->packagingStatusString = QString();
    }

    project->status = ProjectStatus::Idle;

    emit dataChanged( projectIndex, projectIndex, QVector<int>() << StatusRole << PackagingStatusRole << ErrorStatusRole << ErrorStringRole );
    emit projectDownloaded( projectId, project->name, hasError, errorString );
  } );
}

void QFieldCloudProjectsModel::projectDownload( const QString &projectId )
{
  QgsLogger::debug( QStringLiteral( "Project %1: downloading..." ).arg( projectId ) );

  if ( !mCloudConnection )
    return;

  const QModelIndex projectIndex = findProjectIndex( projectId );
  if ( !projectIndex.isValid() )
    return;

  CloudProject *project = mProjects[projectIndex.row()];

  if ( project->isPackagingAborted )
    return;

  NetworkReply *reply = mCloudConnection->get( QStringLiteral( "/api/v1/packages/%1/latest/" ).arg( projectId ) );

  emit dataChanged( projectIndex, projectIndex, QVector<int>() << PackagingStatusRole << StatusRole );

  connect( reply, &NetworkReply::finished, reply, [=]() {
    if ( project->isPackagingAborted )
      return;

    QNetworkReply *rawReply = reply->reply();

    reply->deleteLater();

    if ( rawReply->error() != QNetworkReply::NoError )
    {
      return;
    }

    const QJsonObject payload = QJsonDocument::fromJson( rawReply->readAll() ).object();
    const QString packageId = payload.value( QStringLiteral( "package_id" ) ).toString();
    const QString packagedAt = payload.value( QStringLiteral( "packaged_at" ) ).toString();

    if (
      packageId.isNull()
      || packagedAt.isNull()
      || !payload.value( QStringLiteral( "files" ) ).isArray()
      || !payload.value( QStringLiteral( "layers" ) ).isObject() )
    {
      QgsLogger::debug( QStringLiteral( "JSON structure for `%1` package does not contain the expected fields: package_id(string), packaged_at(string), files(array), layers(object)" ).arg( projectId ) );
      return;
    }

    const QJsonArray files = payload.value( QStringLiteral( "files" ) ).toArray();
    for ( const QJsonValue &fileValue : files )
    {
      QJsonObject fileObject = fileValue.toObject();
      int fileSize = fileObject.value( QStringLiteral( "size" ) ).toInt();
      QString fileName = fileObject.value( QStringLiteral( "name" ) ).toString();
      QString projectFileName = QStringLiteral( "%1/%2/%3/%4" ).arg( QFieldCloudUtils::localCloudDirectory(), mUsername, projectId, fileName );
      QString cloudChecksum = fileObject.value( QStringLiteral( "sha256" ) ).toString();
      QString localChecksum = FileUtils::fileChecksum( projectFileName, QCryptographicHash::Sha256 ).toHex();

      if (
        !fileObject.value( QStringLiteral( "size" ) ).isDouble()
        || fileName.isEmpty()
        || cloudChecksum.isEmpty() )
      {
        QgsLogger::debug( QStringLiteral( "JSON structure for `%1` package in \"files\" list does not contain the expected fields: size(int), name(string), sha256(string)" ).arg( projectId ) );
        return;
      }

      if ( cloudChecksum == localChecksum )
        continue;

      project->downloadFileTransfers.insert( fileName, FileTransfer( fileName, fileSize ) );
      project->downloadBytesTotal += std::max( fileSize, 0 );
    }

    const QJsonObject layers = payload.value( QStringLiteral( "layers" ) ).toObject();
    bool hasLayerExportErrror = false;
    for ( const QString &layerKey : layers.keys() )
    {
      QJsonObject layer = layers.value( layerKey ).toObject();
      QString layerName = layer.value( QStringLiteral( "name" ) ).toString();
      QString layerStatus = layer.value( QStringLiteral( "status" ) ).toString();

      if (
        layerKey.isEmpty()
        || layerName.isEmpty()
        || layerStatus.isEmpty()
        || !layer.value( QStringLiteral( "valid" ) ).isBool() )
      {
        QgsLogger::debug( QStringLiteral( "JSON structure for `%1` package in \"files\" list does not contain the expected fields: name(string), status(string), valid(bool)" ).arg( projectId ) );
        return;
      }

      if ( !layer.value( QStringLiteral( "valid" ) ).toBool() )
      {
        project->packagedLayerErrors.append( tr( "Packaged layer '%1' is not valid: '%2'" ).arg( layerName, layerStatus ) );
        QgsMessageLog::logMessage( project->packagedLayerErrors.last() );

        hasLayerExportErrror = true;
      }
    }

    if ( hasLayerExportErrror )
    {
      QgsLogger::debug( QStringLiteral( "Packaged files list request finished for `%1` with some failed layers:\n%2" ).arg( projectId, project->packagedLayerErrors.join( QStringLiteral( "\n" ) ) ) );
      emit dataChanged( projectIndex, projectIndex, QVector<int>() << PackagedLayerErrorsRole );
    }

    project->lastExportId = packageId;
    project->lastExportedAt = packagedAt;

    QgsLogger::debug( QStringLiteral( "Packaged files to download - %1 files, namely: %2" )
                        .arg( project->downloadFileTransfers.count() )
                        .arg( project->downloadFileTransfers.keys().join( ", " ) ) );

    updateActiveProjectFilesToDownload( projectId );
    projectDownloadFiles( projectId );
  } );
}

void QFieldCloudProjectsModel::updateActiveProjectFilesToDownload( const QString &projectId )
{
  if ( !mCloudConnection )
    return;

  CloudProject *project = findProject( projectId );

  if ( !project )
    return;

  const QStringList fileNames = project->downloadFileTransfers.keys();

  if ( fileNames.isEmpty() )
  {
    mActiveProjectFilesToDownload.clear();
    return;
  }

  for ( const QString &fileName : fileNames )
  {
    if ( project->downloadFileTransfers[fileName].networkReply )
    {
      if ( project->downloadFileTransfers[fileName].networkReply->isFinished() )
      {
        if ( mActiveProjectFilesToDownload.removeOne( fileName ) )
        {
          QgsLogger::debug( QStringLiteral( "Project %1, file `%2`: removed from the list of active download files" ).arg( projectId, fileName ) );
        }
        continue;
      }
      else
      {
        // the request is still active
        continue;
      }
    }

    if ( mActiveProjectFilesToDownload.size() >= MAX_PARALLEL_REQUESTS )
    {
      return;
    }

    QgsLogger::debug( QStringLiteral( "Project %1, file `%2`: appended to the active download files list" ).arg( projectId, fileName ) );

    mActiveProjectFilesToDownload.append( fileName );
  }

  if ( mActiveProjectFilesToDownload.count() > 0 )
  {
    QgsLogger::debug( QStringLiteral( "Project %1: active download files list contains %2 files, namely: %3" ).arg( projectId ).arg( mActiveProjectFilesToDownload.count() ).arg( mActiveProjectFilesToDownload.join( ", " ) ) );
  }
  else
  {
    QgsLogger::debug( QStringLiteral( "Project %1: active download files list is empty" ).arg( projectId ) );
  }
}

void QFieldCloudProjectsModel::projectDownloadFiles( const QString &projectId )
{
  // calling updateActiveProjectFilesToDownload() before calling this function is mandatory

  if ( !mCloudConnection )
    return;

  const QModelIndex projectIndex = findProjectIndex( projectId );

  if ( !projectIndex.isValid() )
    return;
  CloudProject *project = findProject( projectId );

  // Don't call download project files, if there are no project files
  if ( mActiveProjectFilesToDownload.isEmpty() )
  {
    project->status = ProjectStatus::Idle;
    project->downloadProgress = 1;

    emit dataChanged( projectIndex, projectIndex, QVector<int>() << StatusRole << DownloadProgressRole );

    return;
  }

  QgsLogger::debug( QStringLiteral( "Project %1: active download files list before actual download: %2" ).arg( projectId, mActiveProjectFilesToDownload.join( ", " ) ) );

  for ( const QString &fileName : std::as_const( mActiveProjectFilesToDownload ) )
  {
    if ( project->downloadFileTransfers[fileName].networkReply )
    {
      // Download is already in progress
      continue;
    }

    NetworkReply *reply = downloadFile( projectId, fileName );
    QTemporaryFile *file = new QTemporaryFile( reply );

    file->setAutoRemove( false );

    if ( !file->open() )
    {
      project->downloadFilesFailed++;
      projectDownloadFinished( projectId, tr( "Failed to open temporary file for `%1`, reason:\n%2" )
                                            .arg( fileName )
                                            .arg( file->errorString() ) );
      return;
    }

    project->downloadFileTransfers[fileName].tmpFile = file->fileName();
    project->downloadFileTransfers[fileName].networkReply = reply;

    downloadFileConnections( projectId, fileName );
  }
}

bool QFieldCloudProjectsModel::projectMoveDownloadedFilesToPermanentStorage( const QString &projectId )
{
  if ( !mCloudConnection )
    return false;
  CloudProject *project = findProject( projectId );
  if ( !project )
    return false;

  bool hasError = false;
  const QStringList fileNames = project->downloadFileTransfers.keys();

  for ( const QString &fileName : fileNames )
  {
    QFileInfo fileInfo( fileName );
    QFile file( project->downloadFileTransfers[fileName].tmpFile );
    QDir dir( QStringLiteral( "%1/%2/%3/%4" ).arg( QFieldCloudUtils::localCloudDirectory(), mUsername, projectId, fileInfo.path() ) );

    if ( !hasError && !dir.exists() && !dir.mkpath( QStringLiteral( "." ) ) )
    {
      hasError = true;
      QgsMessageLog::logMessage( QStringLiteral( "Failed to create directory at `%1`" ).arg( dir.path() ) );
    }

    const QString destinationFileName( QDir::cleanPath( dir.filePath( fileInfo.fileName() ) ) );

    // if the file already exists, we need to delete it first, as QT does not support overwriting
    // NOTE: it is possible that someone creates the file in the meantime between this and the next if statement
    if ( !hasError && QFile::exists( destinationFileName ) && !file.remove( destinationFileName ) )
    {
      hasError = true;
      QgsMessageLog::logMessage( QStringLiteral( "Failed to remove file before overwriting stored at `%1`, reason:\n%2" ).arg( fileName ).arg( file.errorString() ) );
    }

    if ( !hasError && !file.copy( destinationFileName ) )
    {
      hasError = true;
      QgsMessageLog::logMessage( QStringLiteral( "Failed to write downloaded file stored at `%1`, reason:\n%2" ).arg( fileName ).arg( file.errorString() ) );

      if ( !QFile::remove( dir.filePath( fileName ) ) )
        QgsMessageLog::logMessage( QStringLiteral( "Failed to remove partly overwritten file stored at `%1`" ).arg( fileName ) );
    }

    if ( !file.remove() )
      QgsMessageLog::logMessage( QStringLiteral( "Failed to remove temporary file `%1`" ).arg( fileName ) );
  }

  return !hasError;
}


void QFieldCloudProjectsModel::uploadProject( const QString &projectId, const bool shouldDownloadUpdates )
{
  const QModelIndex projectIndex = findProjectIndex( projectId );

  if ( !projectIndex.isValid() )
    return;

  CloudProject *project = mProjects[projectIndex.row()];

  if ( !( project->status == ProjectStatus::Idle ) )
    return;

  DeltaFileWrapper *deltaFileWrapper = mLayerObserver->deltaFileWrapper();

  if ( shouldDownloadUpdates && deltaFileWrapper->count() == 0 )
  {
    project->status = ProjectStatus::Idle;
    projectPackageAndDownload( projectId );
    return;
  }

  if ( !( project->modification & LocalModification ) )
    return;

  if ( !mLayerObserver->deltaFileWrapper()->toFile() )
    return;

  if ( deltaFileWrapper->hasError() )
  {
    QgsMessageLog::logMessage( QStringLiteral( "The delta file has an error: %1" ).arg( deltaFileWrapper->errorString() ) );
    return;
  }

  project->status = ProjectStatus::Uploading;
  project->deltaFileId = deltaFileWrapper->id();
  project->deltaFileUploadStatus = DeltaLocalStatus;
  project->deltaFileUploadStatusString = QString();

  refreshProjectModification( projectId );

  emit dataChanged( projectIndex, projectIndex, QVector<int>() << StatusRole << UploadAttachmentsCountRole << UploadDeltaProgressRole << UploadDeltaStatusRole << UploadDeltaStatusStringRole );

  // //////////
  // prepare attachment files to be uploaded
  // //////////
  if ( project->uploadAttachmentsStatus != UploadAttachmentsStatus::UploadAttachmentsDone )
  {
    projectCancelUploadAttachments( projectId );
  }
  project->uploadAttachmentsFailed = 0;

  const QFileInfo projectInfo( QFieldCloudUtils::localProjectFilePath( mUsername, projectId ) );
  const QDir projectDir( projectInfo.absolutePath() );
  const QStringList attachmentFileNames = deltaFileWrapper->attachmentFileNames().keys();
  for ( const QString &fileName : attachmentFileNames )
  {
    QString absoluteFilePath = fileName;
    QFileInfo fileInfo( fileName );
    if ( fileInfo.isRelative() )
    {
      absoluteFilePath = projectDir.absoluteFilePath( fileName );
      fileInfo = QFileInfo( absoluteFilePath );
    }

    if ( !fileInfo.exists() || !fileInfo.isFile() )
    {
      QgsMessageLog::logMessage( QStringLiteral( "Attachment file '%1' does not exist" ).arg( absoluteFilePath ) );
      continue;
    }

    const long long fileSize = fileInfo.size();

    // ? should we also check the checksums of the files being uploaded? they are available at deltaFile->attachmentFileNames()->values()
    project->uploadAttachments.insert( absoluteFilePath, FileTransfer( absoluteFilePath, fileSize ) );
  }

  QFieldCloudUtils::setProjectSetting( projectId, QStringLiteral( "uploadAttachments" ), QStringList( project->uploadAttachments.keys() ) );

  QString deltaFileToUpload = deltaFileWrapper->toFileForUpload();

  if ( deltaFileToUpload.isEmpty() )
  {
    project->status = ProjectStatus::Idle;
    emit dataChanged( projectIndex, projectIndex, QVector<int>() << StatusRole );
    return;
  }

  // //////////
  // 1) upload the deltas
  // //////////
  NetworkReply *deltasCloudReply = mCloudConnection->post(
    QStringLiteral( "/api/v1/deltas/%1/" ).arg( projectId ),
    QVariantMap(),
    QStringList( { deltaFileToUpload } ) );

  Q_ASSERT( deltasCloudReply );

  connect( deltasCloudReply, &NetworkReply::uploadProgress, this, [=]( int bytesSent, int bytesTotal ) {
    project->uploadDeltaProgress = std::clamp( ( static_cast<double>( bytesSent ) / bytesTotal ), 0., 1. );

    emit dataChanged( projectIndex, projectIndex, QVector<int>() << UploadDeltaProgressRole );
  } );
  connect( deltasCloudReply, &NetworkReply::finished, this, [=]() {
    QNetworkReply *deltasReply = deltasCloudReply->reply();
    deltasCloudReply->deleteLater();

    Q_ASSERT( deltasCloudReply->isFinished() );
    Q_ASSERT( deltasReply );

    // if there is an error, cannot continue sync
    if ( deltasReply->error() != QNetworkReply::NoError )
    {
      project->deltaFileUploadStatusString = QFieldCloudConnection::errorString( deltasReply );
      // TODO check why exactly we failed
      // maybe the project does not exist, then create it?
      QgsMessageLog::logMessage( QStringLiteral( "Failed to upload delta file, reason:\n%1\n%2" ).arg( deltasReply->errorString(), project->deltaFileUploadStatusString ) );
      projectCancelUpload( projectId );
      return;
    }

    project->uploadDeltaProgress = 1;
    project->deltaFileUploadStatus = DeltaPendingStatus;
    project->deltaLayersToDownload = deltaFileWrapper->deltaLayerIds();

    emit dataChanged( projectIndex, projectIndex, QVector<int>() << UploadDeltaProgressRole << UploadDeltaStatusRole );
    emit networkDeltaUploaded( projectId );
  } );


  // //////////
  // 2) delta successfully uploaded
  // //////////
  QObject *networkDeltaUploadedParent = new QObject( this ); // we need this to unsubscribe
  connect( this, &QFieldCloudProjectsModel::networkDeltaUploaded, networkDeltaUploadedParent, [=]( const QString &uploadedProjectId ) {
    if ( projectId != uploadedProjectId )
      return;

    delete networkDeltaUploadedParent;

    // send attachment in a non-blocking fashion
    projectUploadAttachments( projectId );

    if ( shouldDownloadUpdates )
    {
      projectApplyDeltas( projectId );
    }
    else
    {
      project->status = ProjectStatus::Idle;
      project->modification ^= LocalModification;

      deltaFileWrapper->reset();
      deltaFileWrapper->resetId();

      if ( !deltaFileWrapper->toFile() )
        QgsMessageLog::logMessage( QStringLiteral( "Failed to reset delta file after delta push. %1" ).arg( deltaFileWrapper->errorString() ) );

      emit dataChanged( projectIndex, projectIndex, QVector<int>() << StatusRole );
      emit pushFinished( projectId, false );
    }
  } );

  // //////////
  // 3) new delta status received. Never give up to get a successful status.
  // //////////
  QObject *networkDeltaStatusCheckedParent = new QObject( this ); // we need this to unsubscribe
  connect( this, &QFieldCloudProjectsModel::networkDeltaStatusChecked, networkDeltaStatusCheckedParent, [=]( const QString &uploadedProjectId ) {
    if ( projectId != uploadedProjectId )
      return;

    switch ( project->deltaFileUploadStatus )
    {
      case DeltaLocalStatus:
        // delta file should be already sent!!!
        Q_ASSERT( 0 );
        FALLTHROUGH
      case DeltaPendingStatus:
      case DeltaBusyStatus:
        // infinite retry, there should be one day, when we can get the status!
        QTimer::singleShot( sDelayBeforeStatusRetry, [=]() {
          projectGetDeltaStatus( projectId );
        } );
        break;
      case DeltaErrorStatus:
        delete networkDeltaStatusCheckedParent;
        deltaFileWrapper->resetId();

        if ( !deltaFileWrapper->toFile() )
          QgsMessageLog::logMessage( QStringLiteral( "Failed update committed delta file." ) );

        projectCancelUpload( projectId );
        return;
      case DeltaConflictStatus:
      case DeltaNotAppliedStatus:
      case DeltaAppliedStatus:
        delete networkDeltaStatusCheckedParent;

        deltaFileWrapper->reset();
        deltaFileWrapper->resetId();

        if ( !deltaFileWrapper->toFile() )
          QgsMessageLog::logMessage( QStringLiteral( "Failed to reset delta file. %1" ).arg( deltaFileWrapper->errorString() ) );

        project->status = ProjectStatus::Idle;
        project->modification ^= LocalModification;
        project->modification |= RemoteModification;

        project->lastLocalPushDeltas = QDateTime::currentDateTimeUtc().toString( Qt::ISODate );
        QFieldCloudUtils::setProjectSetting( projectId, QStringLiteral( "lastLocalPushDeltas" ), project->lastLocalPushDeltas );

        emit dataChanged( projectIndex, projectIndex, QVector<int>() << ModificationRole << LastLocalPushDeltasRole );

        // download the updated files, so the files are for sure the same on the client and on the server
        if ( shouldDownloadUpdates )
        {
          projectPackageAndDownload( projectId );
        }
        else
        {
          emit dataChanged( projectIndex, projectIndex, QVector<int>() << StatusRole );
          emit pushFinished( projectId, false );
        }
    }
  } );


  // this code is no longer needed, as we do not upload or download files selectively
  // !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
  //  // //////////
  //  // 4) project downloaded, if all done, then reload the project and sync done!
  //  // //////////
  //  connect( this, &QFieldCloudProjectsModel::networkAllLayersDownloaded, this, [ = ]( const QString & callerProjectId )
  //  {
  //    if ( projectId != callerProjectId )
  //      return;

  //    // wait until all layers are downloaded
  //    if ( project->downloadLayersFinished < project->deltaLayersToDownload.size() )
  //      return;

  //    // there are some files that failed to download
  //    if ( project->downloadLayersFailed > 0 )
  //    {
  //      // TODO translate this message
  //      project->deltaFileUploadStatusString = QStringLiteral( "Failed to retrieve some of the updated layers, but changes are committed on the server. "
  //                            "Try to reload the project from the cloud." );
  //      projectCancelUpload( projectId );

  //      return;
  //    }

  //    QgsProject::instance()->reloadAllLayers();

  //    emit dataChanged( projectIndex, projectIndex, QVector<int>() << StatusRole );
  //    emit syncFinished( projectId, false );
  //  } );
}


void QFieldCloudProjectsModel::projectApplyDeltas( const QString &projectId )
{
  const QModelIndex projectIndex = findProjectIndex( projectId );

  if ( !projectIndex.isValid() )
    return;

  CloudProject *project = mProjects[projectIndex.row()];
  NetworkReply *reply = mCloudConnection->post( QStringLiteral( "/api/v1/deltas/apply/%1/" ).arg( project->id ) );

  connect( reply, &NetworkReply::finished, this, [=]() {
    QNetworkReply *rawReply = reply->reply();
    reply->deleteLater();

    Q_ASSERT( reply->isFinished() );
    Q_ASSERT( rawReply );

    if ( rawReply->error() != QNetworkReply::NoError )
    {
      project->status = ProjectStatus::Idle;
      project->deltaFileUploadStatus = DeltaErrorStatus;
      project->deltaFileUploadStatusString = QFieldCloudConnection::errorString( rawReply );

      emit dataChanged( projectIndex, projectIndex, QVector<int>() << UploadDeltaStatusRole << UploadDeltaStatusStringRole );
      emit networkDeltaStatusChecked( projectId );
      return;
    }

    projectGetDeltaStatus( projectId );
  } );
}

void QFieldCloudProjectsModel::refreshProjectDeltaList( const QString &projectId )
{
  const QModelIndex projectIndex = findProjectIndex( projectId );
  if ( !projectIndex.isValid() )
    return;
  CloudProject *project = mProjects[projectIndex.row()];
  NetworkReply *deltaStatusReply = mCloudConnection->get( QStringLiteral( "/api/v1/deltas/%1/" ).arg( project->id ) );

  if ( project->deltaListModel )
  {
    delete project->deltaListModel;
    project->deltaListModel = nullptr;
    emit deltaListModelChanged();
  }

  connect( deltaStatusReply, &NetworkReply::finished, this, [=]() {
    QNetworkReply *rawReply = deltaStatusReply->reply();
    deltaStatusReply->deleteLater();

    Q_ASSERT( deltaStatusReply->isFinished() );
    Q_ASSERT( rawReply );

    if ( rawReply->error() != QNetworkReply::NoError )
    {
      return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson( rawReply->readAll() );

    project->deltaListModel = new DeltaListModel( doc );

    emit dataChanged( projectIndex, projectIndex, QVector<int>() << DeltaListRole );
    emit deltaListModelChanged();
  } );
}

void QFieldCloudProjectsModel::projectGetDeltaStatus( const QString &projectId )
{
  const QModelIndex projectIndex = findProjectIndex( projectId );
  if ( !projectIndex.isValid() )
    return;
  CloudProject *project = mProjects[projectIndex.row()];
  NetworkReply *deltaStatusReply = mCloudConnection->get( QStringLiteral( "/api/v1/deltas/%1/%2/" ).arg( project->id, project->deltaFileId ) );

  project->deltaFileUploadStatusString = QString();
  connect( deltaStatusReply, &NetworkReply::finished, this, [=]() {
    QNetworkReply *rawReply = deltaStatusReply->reply();
    deltaStatusReply->deleteLater();

    Q_ASSERT( deltaStatusReply->isFinished() );
    Q_ASSERT( rawReply );

    CloudProject *project = findProject( projectId );

    if ( rawReply->error() != QNetworkReply::NoError )
    {
      project->deltaFileUploadStatus = DeltaErrorStatus;
      // TODO this is oversimplification. e.g. 404 error is when the requested delta file id is not existant
      project->deltaFileUploadStatusString = QFieldCloudConnection::errorString( rawReply );

      emit dataChanged( projectIndex, projectIndex, QVector<int>() << UploadDeltaStatusRole << UploadDeltaStatusStringRole );
      emit networkDeltaStatusChecked( projectId );

      return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson( rawReply->readAll() );
    DeltaListModel deltaListModel( doc );
    if ( !deltaListModel.isValid() )
    {
      project->deltaFileUploadStatus = DeltaErrorStatus;
      project->deltaFileUploadStatusString = deltaListModel.errorString();
      emit dataChanged( projectIndex, projectIndex, QVector<int>() << UploadDeltaStatusRole << UploadDeltaStatusStringRole );
      emit networkDeltaStatusChecked( projectId );
      return;
    }

    project->deltaFileUploadStatusString = QString();

    if ( !deltaListModel.allHaveFinalStatus() )
    {
      project->deltaFileUploadStatus = DeltaPendingStatus;
      emit dataChanged( projectIndex, projectIndex, QVector<int>() << UploadDeltaStatusRole << UploadDeltaStatusStringRole );
      emit networkDeltaStatusChecked( projectId );
      return;
    }

    project->deltaFileUploadStatus = DeltaAppliedStatus;

    emit dataChanged( projectIndex, projectIndex, QVector<int>() << UploadDeltaStatusRole << UploadDeltaStatusStringRole );
    emit networkDeltaStatusChecked( projectId );
  } );
}

void QFieldCloudProjectsModel::projectUploadAttachments( const QString &projectId )
{
  const QModelIndex projectIndex = findProjectIndex( projectId );
  if ( !projectIndex.isValid() )
    return;
  CloudProject *project = mProjects[projectIndex.row()];

  if ( project->uploadAttachments.size() == 0 )
    return;

  // start uploading the attachments
  const QStringList attachmentFileNames = project->uploadAttachments.keys();
  for ( const QString &fileName : attachmentFileNames )
  {
    NetworkReply *attachmentCloudReply = uploadAttachment( projectId, fileName );
    project->uploadAttachments[fileName].networkReply = attachmentCloudReply;

    connect( attachmentCloudReply, &NetworkReply::uploadProgress, this, [=]( int bytesSent, int bytesTotal ) {
      Q_UNUSED( bytesTotal )
      project->uploadAttachments[fileName].bytesTransferred = bytesSent;
      emit dataChanged( projectIndex, projectIndex, QVector<int>() << UploadAttachmentsCountRole );
    } );

    connect( attachmentCloudReply, &NetworkReply::finished, this, [=]() {
      QNetworkReply *attachmentReply = attachmentCloudReply->reply();
      attachmentCloudReply->deleteLater();

      Q_ASSERT( attachmentCloudReply->isFinished() );
      Q_ASSERT( attachmentReply );

      // if there is an error, don't panic, we continue uploading. The files may be later manually synced.
      if ( attachmentReply->error() != QNetworkReply::NoError )
      {
        project->uploadAttachmentsFailed++;
        QgsMessageLog::logMessage( tr( "Failed to upload attachment stored at `%1`, reason:\n%2" )
                                     .arg( fileName )
                                     .arg( QFieldCloudConnection::errorString( attachmentReply ) ) );
      }
      else
      {
        project->uploadAttachments.remove( fileName );
        QFieldCloudUtils::setProjectSetting( projectId, QStringLiteral( "uploadAttachments" ), QStringList( project->uploadAttachments.keys() ) );
      }

      if ( project->uploadAttachments.size() - project->uploadAttachmentsFailed == 0 )
        project->uploadAttachmentsStatus = UploadAttachmentsStatus::UploadAttachmentsDone;

      emit dataChanged( projectIndex, projectIndex, QVector<int>() << UploadAttachmentsCountRole );
    } );
  }
  if ( attachmentFileNames.size() > 0 )
  {
    project->uploadAttachmentsStatus = UploadAttachmentsStatus::UploadAttachmentsInProgress;
    emit dataChanged( projectIndex, projectIndex, QVector<int>() << UploadAttachmentsStatusRole );
  }
}

void QFieldCloudProjectsModel::projectCancelUpload( const QString &projectId )
{
  const QModelIndex projectIndex = findProjectIndex( projectId );
  if ( !projectIndex.isValid() )
    return;
  CloudProject *project = mProjects[projectIndex.row()];

  projectCancelUploadAttachments( projectId );

  project->status = ProjectStatus::Idle;
  project->errorStatus = UploadErrorStatus;

  emit dataChanged( projectIndex, projectIndex, QVector<int>() << StatusRole << ErrorStatusRole );
  emit pushFinished( projectId, true, project->deltaFileUploadStatusString );

  return;
}

void QFieldCloudProjectsModel::projectCancelUploadAttachments( const QString &projectId )
{
  const QModelIndex projectIndex = findProjectIndex( projectId );
  if ( !projectIndex.isValid() )
    return;

  CloudProject *project = mProjects[projectIndex.row()];

  const QStringList attachmentFileNames = project->uploadAttachments.keys();
  for ( const QString &attachmentFileName : attachmentFileNames )
  {
    NetworkReply *attachmentReply = project->uploadAttachments[attachmentFileName].networkReply;

    // the replies might be already disposed
    if ( !attachmentReply )
      continue;

    // the replies might be already finished
    if ( attachmentReply->isFinished() )
      continue;

    attachmentReply->abort();
  }
  project->uploadAttachmentsStatus = UploadAttachmentsStatus::UploadAttachmentsDone;
  emit dataChanged( projectIndex, projectIndex, QVector<int>() << UploadAttachmentsStatusRole );
}

void QFieldCloudProjectsModel::connectionStatusChanged()
{
  refreshProjectsList();
}

void QFieldCloudProjectsModel::layerObserverLayerEdited( const QString &layerId )
{
  Q_UNUSED( layerId )
  CloudProject *project = findProject( mCurrentProjectId );

  if ( !project )
  {
    QgsMessageLog::logMessage( QStringLiteral( "Layer observer triggered `isDirtyChanged` signal incorrectly" ) );
    return;
  }

  beginResetModel();

  const DeltaFileWrapper *deltaFileWrapper = mLayerObserver->deltaFileWrapper();

  Q_ASSERT( deltaFileWrapper );

  if ( deltaFileWrapper->count() > 0 )
    project->modification |= LocalModification;
  else if ( project->modification & LocalModification )
    project->modification ^= LocalModification;

  endResetModel();
}

void QFieldCloudProjectsModel::projectListReceived()
{
  NetworkReply *reply = qobject_cast<NetworkReply *>( sender() );
  QNetworkReply *rawReply = reply->reply();

  Q_ASSERT( rawReply );

  if ( rawReply->error() != QNetworkReply::NoError )
  {
    emit warning( QFieldCloudConnection::errorString( rawReply ) );
    return;
  }

  QByteArray response = rawReply->readAll();

  QJsonDocument doc = QJsonDocument::fromJson( response );
  QJsonArray projects = doc.array();
  reload( projects );
}

NetworkReply *QFieldCloudProjectsModel::downloadFile( const QString &projectId, const QString &fileName )
{
  QNetworkRequest request;
  request.setAttribute( QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::RedirectPolicy::UserVerifiedRedirectPolicy );
  mCloudConnection->setAuthenticationToken( request );

  return mCloudConnection->get( request, QStringLiteral( "/api/v1/packages/%1/latest/files/%2" ).arg( projectId, fileName ) );
}


void QFieldCloudProjectsModel::downloadFileConnections( const QString &projectId, const QString &fileName )
{
  const QModelIndex projectIndex = findProjectIndex( projectId );

  if ( !projectIndex.isValid() )
    return;
  CloudProject *project = findProject( projectId );

  if ( !project->downloadFileTransfers.contains( fileName ) )
  {
    Q_ASSERT( false );
    return;
  }

  NetworkReply *reply = project->downloadFileTransfers[fileName].networkReply;

  if ( !reply )
  {
    Q_ASSERT( false );
    return;
  }

  QStringList fileNames = project->downloadFileTransfers.keys();

  QgsLogger::debug( QStringLiteral( "Project %1, id `%2`: requested." ).arg( projectId, fileName ) );

  connect( reply, &NetworkReply::redirected, reply, [=]( const QUrl &url ) {
    QUrl oldUrl = project->downloadFileTransfers[fileName].lastRedirectUrl;

    project->downloadFileTransfers[fileName].redirectsCount++;
    project->downloadFileTransfers[fileName].lastRedirectUrl = url;

    if ( project->downloadFileTransfers[fileName].redirectsCount >= MAX_REDIRECTS_ALLOWED )
    {
      QgsLogger::debug( QStringLiteral( "Project %1, file `%2`: too many redirects, last two urls are `%3` and `%4`" ).arg( projectId, fileName, oldUrl.toString(), url.toString() ) );
      reply->abort();
      return;
    }

    if ( oldUrl == url )
    {
      QgsLogger::debug( QStringLiteral( "Project %1, file `%2`: has redirects to the same URL `%3`" ).arg( projectId, fileName, url.toString() ) );
      reply->abort();
      return;
    }

    QgsLogger::debug( QStringLiteral( "Package %1, file `%2`: redirected to `%3`" ).arg( projectId, fileName, url.toString() ) );

    QNetworkRequest request;
    project->downloadFileTransfers[fileName].networkReply = mCloudConnection->get( request, url );

    // we need to somehow finish the request, otherwise it will remain unfinished for the QFieldCloudConnection
    reply->abort();

    downloadFileConnections( projectId, fileName );
  } );

  connect( reply, &NetworkReply::downloadProgress, reply, [=]( int bytesReceived, int bytesTotal ) {
    Q_UNUSED( bytesTotal )

    // it means the NetworkReply has failed and retried
    project->downloadBytesReceived -= project->downloadFileTransfers[fileName].bytesTransferred;
    project->downloadBytesReceived += bytesReceived;
    project->downloadFileTransfers[fileName].bytesTransferred = bytesReceived;
    project->downloadProgress = std::clamp( ( static_cast<double>( project->downloadBytesReceived ) / std::max( project->downloadBytesTotal, 1 ) ), 0., 1. );

    emit dataChanged( projectIndex, projectIndex, QVector<int>() << DownloadProgressRole );
  } );

  connect( reply, &NetworkReply::finished, reply, [=]() {
    if ( project->packagingStatus == PackagingAbortStatus )
      return;

    QVector<int> rolesChanged;
    QNetworkReply *rawReply = reply->reply();

    Q_ASSERT( reply->isFinished() );
    Q_ASSERT( reply );

    // this is most probably the redirected request, nothing to do with this reply anymore, just ignore it
    if ( project->downloadFileTransfers[fileName].networkReply != reply )
      return;

    project->downloadFilesFinished++;

    bool hasError = false;
    QString errorMessageDetail;
    QString errorMessageTemplate;

    if ( rawReply->error() != QNetworkReply::NoError )
    {
      hasError = true;
      errorMessageDetail = QFieldCloudConnection::errorString( rawReply );
      errorMessageTemplate = tr( "Failed to download file `%1`, reason:" ).arg( fileName );
    }

    QFile file( project->downloadFileTransfers[fileName].tmpFile );

    file.open( QIODevice::ReadWrite );

    if ( !hasError && !file.write( rawReply->readAll() ) )
    {
      hasError = true;
      errorMessageDetail = file.errorString();
      errorMessageTemplate = tr( "Failed to write downloaded file stored at `%1`, fs reason:" ).arg( project->downloadFileTransfers[fileName].tmpFile );
    }

    if ( hasError )
    {
      QString trimmedErrorMessage = errorMessageDetail.size() > 100
                                      ? ( errorMessageDetail.left( 100 ) + tr( " (see more in the QField error log)…" ) )
                                      : errorMessageDetail;
      project->downloadFilesFailed++;

      QString errorMessage = QStringLiteral( "%1: \n%2" ).arg( errorMessageTemplate, fileName ).arg( trimmedErrorMessage );

      QgsLogger::debug( QStringLiteral( "%1: \n%2" ).arg( errorMessageTemplate, fileName ).arg( errorMessageDetail ) );
      QgsMessageLog::logMessage( QStringLiteral( "%1: \n%2" ).arg( errorMessageTemplate, fileName ).arg( errorMessageDetail ) );

      emit projectDownloadFinished( projectId, errorMessage );

      return;
    }

    QgsLogger::debug( QStringLiteral( "File `%1` downloaded." ).arg( fileName ) );

    updateActiveProjectFilesToDownload( projectId );

    if ( project->downloadFilesFinished == fileNames.count() )
    {
      QgsLogger::debug( QStringLiteral( "All files downloaded." ) );

      Q_ASSERT( mActiveProjectFilesToDownload.size() == 0 );

      if ( !hasError )
      {
        const QStringList unprefixedGpkgFileNames = filterGpkgFileNames( fileNames );
        const QStringList gpkgFileNames = projectFileNames( mProject->homePath(), unprefixedGpkgFileNames );
        // we need to close the project to safely flush the gpkg files
        mProject->setFileName( QString() );

        for ( const QString &fileName : gpkgFileNames )
          mGpkgFlusher->stop( fileName );

        // move the files from their temporary location to their permanent one
        if ( !projectMoveDownloadedFilesToPermanentStorage( projectId ) )
        {
          emit projectDownloadFinished( projectId, tr( "Failed to copy some of the downloaded files on your device. Check your device storage." ) );
          return;
        }

        deleteGpkgShmAndWal( gpkgFileNames );

        for ( const QString &fileName : gpkgFileNames )
          mGpkgFlusher->start( fileName );

        project->errorStatus = NoErrorStatus;
        project->packagingStatus = PackagingFinishedStatus;
        project->packagingStatusString = QString();
        project->checkout = ProjectCheckout::LocalAndRemoteCheckout;
        project->localPath = QFieldCloudUtils::localProjectFilePath( mUsername, projectId );
        project->lastLocalExportedAt = QDateTime::currentDateTimeUtc().toString( Qt::ISODate );
        project->lastLocalExportId = QUuid::createUuid().toString( QUuid::WithoutBraces );
        QFieldCloudUtils::setProjectSetting( projectId, QStringLiteral( "lastExportedAt" ), project->lastExportedAt );
        QFieldCloudUtils::setProjectSetting( projectId, QStringLiteral( "lastExportId" ), project->lastExportId );
        QFieldCloudUtils::setProjectSetting( projectId, QStringLiteral( "lastLocalExportedAt" ), project->lastLocalExportedAt );
        QFieldCloudUtils::setProjectSetting( projectId, QStringLiteral( "lastLocalExportId" ), project->lastLocalExportId );

        emit projectDownloadFinished( projectId );
      }

      for ( const QString &fileNameKey : fileNames )
      {
        project->downloadFileTransfers[fileNameKey].networkReply->deleteLater();
      }

      project->status = ProjectStatus::Idle;
    }
    else
    {
      projectDownloadFiles( projectId );
    }

    rolesChanged << StatusRole << LocalPathRole << CheckoutRole << LastLocalExportedAtRole;

    emit dataChanged( projectIndex, projectIndex, rolesChanged );
  } );
}

NetworkReply *QFieldCloudProjectsModel::uploadAttachment( const QString &projectId, const QString &fileName )
{
  QFileInfo projectInfo( QFieldCloudUtils::localProjectFilePath( mUsername, projectId ) );
  QDir projectDir( projectInfo.absolutePath() );

  const QString apiPath = projectDir.relativeFilePath( fileName );

  return mCloudConnection->post( QStringLiteral( "/api/v1/files/%1/%2/" ).arg( projectId, apiPath ), QVariantMap(), QStringList( { fileName } ) );
}

QHash<int, QByteArray> QFieldCloudProjectsModel::roleNames() const
{
  QHash<int, QByteArray> roles;
  roles[IdRole] = "Id";
  roles[PrivateRole] = "Private";
  roles[OwnerRole] = "Owner";
  roles[NameRole] = "Name";
  roles[DescriptionRole] = "Description";
  roles[ModificationRole] = "Modification";
  roles[CheckoutRole] = "Checkout";
  roles[StatusRole] = "Status";
  roles[ErrorStatusRole] = "ErrorStatus";
  roles[ErrorStringRole] = "ErrorString";
  roles[DownloadProgressRole] = "DownloadProgress";
  roles[PackagingStatusRole] = "PackagingStatus";
  roles[PackagedLayerErrorsRole] = "PackagedLayerErrors";
  roles[UploadAttachmentsStatusRole] = "UploadAttachmentsStatus";
  roles[UploadAttachmentsCountRole] = "UploadAttachmentsCount";
  roles[UploadDeltaProgressRole] = "UploadDeltaProgress";
  roles[UploadDeltaStatusRole] = "UploadDeltaStatus";
  roles[UploadDeltaStatusStringRole] = "UploadDeltaStatusString";
  roles[LocalDeltasCountRole] = "LocalDeltasCount";
  roles[LocalPathRole] = "LocalPath";
  roles[CanSyncRole] = "CanSync";
  roles[LastLocalExportedAtRole] = "LastLocalExportedAt";
  roles[LastLocalPushDeltasRole] = "LastLocalPushDeltas";
  roles[UserRoleRole] = "UserRole";
  roles[DeltaListRole] = "DeltaList";

  return roles;
}

void QFieldCloudProjectsModel::reload( const QJsonArray &remoteProjects )
{
  beginResetModel();
  mProjects.clear();

  QgsProject *qgisProject = QgsProject::instance();

  auto restoreLocalSettings = [=]( CloudProject *cloudProject, const QDir &localPath ) {
    cloudProject->deltasCount = DeltaFileWrapper( qgisProject, QStringLiteral( "%1/deltafile.json" ).arg( localPath.absolutePath() ) ).count();
    cloudProject->lastExportId = QFieldCloudUtils::projectSetting( cloudProject->id, QStringLiteral( "lastExportId" ) ).toString();
    cloudProject->lastExportedAt = QFieldCloudUtils::projectSetting( cloudProject->id, QStringLiteral( "lastExportedAt" ) ).toString();
    cloudProject->lastLocalExportId = QFieldCloudUtils::projectSetting( cloudProject->id, QStringLiteral( "lastLocalExportId" ) ).toString();
    cloudProject->lastLocalExportedAt = QFieldCloudUtils::projectSetting( cloudProject->id, QStringLiteral( "lastLocalExportedAt" ) ).toString();
    cloudProject->lastLocalPushDeltas = QFieldCloudUtils::projectSetting( cloudProject->id, QStringLiteral( "lastLocalPushDeltas" ) ).toString();

    // generate local export id if not present. Possible reasons for missing localExportId are:
    // - just upgraded QField that introduced the field
    // - the local settings were somehow deleted, but not the project itself
    if ( cloudProject->lastLocalExportId.isEmpty() )
    {
      cloudProject->lastLocalExportId = QUuid::createUuid().toString( QUuid::WithoutBraces );
      QFieldCloudUtils::setProjectSetting( cloudProject->id, QStringLiteral( "lastLocalExportId" ), cloudProject->lastLocalExportId );
    }

    const QStringList fileNames = QFieldCloudUtils::projectSetting( cloudProject->id, QStringLiteral( "uploadAttachments" ) ).toStringList();
    for ( const QString &fileName : fileNames )
    {
      QFileInfo fileInfo( fileName );
      if ( fileInfo.exists() && !fileInfo.isDir() )
      {
        cloudProject->uploadAttachments.insert( fileName, FileTransfer( fileName, fileInfo.size() ) );
      }
    }
  };

  for ( const auto project : remoteProjects )
  {
    QVariantHash projectDetails = project.toObject().toVariantHash();
    CloudProject *cloudProject = new CloudProject( projectDetails.value( "id" ).toString(),
                                                   projectDetails.value( "private" ).toBool(),
                                                   projectDetails.value( "owner" ).toString(),
                                                   projectDetails.value( "name" ).toString(),
                                                   projectDetails.value( "description" ).toString(),
                                                   projectDetails.value( "user_role" ).toString(),
                                                   QString(),
                                                   RemoteCheckout,
                                                   ProjectStatus::Idle,
                                                   projectDetails.value( "can_repackage" ).toBool(),
                                                   projectDetails.value( "needs_repackaging" ).toBool() );

    const QString projectPrefix = QStringLiteral( "QFieldCloud/projects/%1" ).arg( cloudProject->id );
    QFieldCloudUtils::setProjectSetting( cloudProject->id, QStringLiteral( "owner" ), cloudProject->owner );
    QFieldCloudUtils::setProjectSetting( cloudProject->id, QStringLiteral( "name" ), cloudProject->name );
    QFieldCloudUtils::setProjectSetting( cloudProject->id, QStringLiteral( "description" ), cloudProject->description );
    QFieldCloudUtils::setProjectSetting( cloudProject->id, QStringLiteral( "updatedAt" ), cloudProject->updatedAt );
    QFieldCloudUtils::setProjectSetting( cloudProject->id, QStringLiteral( "userRole" ), cloudProject->userRole );
    QFieldCloudUtils::setProjectSetting( cloudProject->id, QStringLiteral( "canRepackage" ), cloudProject->canRepackage );
    QFieldCloudUtils::setProjectSetting( cloudProject->id, QStringLiteral( "needsRepackaging" ), cloudProject->needsRepackaging );

    if ( !mUsername.isEmpty() )
    {
      cloudProject->localPath = QFieldCloudUtils::localProjectFilePath( mUsername, cloudProject->id );
      QDir localPath( QStringLiteral( "%1/%2/%3" ).arg( QFieldCloudUtils::localCloudDirectory(), mUsername, cloudProject->id ) );
      if ( localPath.exists() )
      {
        cloudProject->checkout = LocalAndRemoteCheckout;
        restoreLocalSettings( cloudProject, localPath );
      }
    }

    cloudProject->lastRefreshedAt = QDateTime::currentDateTimeUtc();

    mProjects << cloudProject;
  }

  QDirIterator userDirs( QFieldCloudUtils::localCloudDirectory(), QDir::Dirs | QDir::NoDotAndDotDot );
  while ( userDirs.hasNext() )
  {
    userDirs.next();
    const QString username = userDirs.fileName();

    // We skip cloud projects that are not linked to the last successul logged in account
    if ( username != mUsername )
      continue;

    QDirIterator projectDirs( QStringLiteral( "%1/%2" ).arg( QFieldCloudUtils::localCloudDirectory(), username ), QDir::Dirs | QDir::NoDotAndDotDot );
    while ( projectDirs.hasNext() )
    {
      projectDirs.next();

      const QString projectId = projectDirs.fileName();
      CloudProject *project = findProject( projectId );
      if ( project )
        continue;

      const QString projectPrefix = QStringLiteral( "QFieldCloud/projects/%1" ).arg( projectId );
      if ( !QSettings().contains( QStringLiteral( "%1/name" ).arg( projectPrefix ) ) )
        continue;

      const QString owner = QFieldCloudUtils::projectSetting( projectId, QStringLiteral( "owner" ) ).toString();
      const QString name = QFieldCloudUtils::projectSetting( projectId, QStringLiteral( "name" ) ).toString();
      const QString description = QFieldCloudUtils::projectSetting( projectId, QStringLiteral( "description" ) ).toString();
      const QString updatedAt = QFieldCloudUtils::projectSetting( projectId, QStringLiteral( "updatedAt" ) ).toString();
      const QString userRole = QFieldCloudUtils::projectSetting( projectId, QStringLiteral( "userRole" ) ).toString();

      CloudProject *cloudProject = new CloudProject( projectId, true, owner, name, description, userRole, QString(), LocalCheckout, ProjectStatus::Idle, false, false );

      cloudProject->localPath = QFieldCloudUtils::localProjectFilePath( username, cloudProject->id );
      QDir localPath( QStringLiteral( "%1/%2/%3" ).arg( QFieldCloudUtils::localCloudDirectory(), username, cloudProject->id ) );
      restoreLocalSettings( cloudProject, localPath );

      mProjects << cloudProject;

      Q_ASSERT( projectId == cloudProject->id );
    }
  }

  endResetModel();
}

int QFieldCloudProjectsModel::rowCount( const QModelIndex &parent ) const
{
  if ( !parent.isValid() )
    return mProjects.size();
  else
    return 0;
}

QVariant QFieldCloudProjectsModel::data( const QModelIndex &index, int role ) const
{
  if ( index.row() >= mProjects.size() || index.row() < 0 )
    return QVariant();

  switch ( static_cast<ColumnRole>( role ) )
  {
    case IdRole:
      return mProjects.at( index.row() )->id;
    case PrivateRole:
      return mProjects.at( index.row() )->isPrivate;
    case OwnerRole:
      return mProjects.at( index.row() )->owner;
    case NameRole:
      return mProjects.at( index.row() )->name;
    case DescriptionRole:
      return mProjects.at( index.row() )->description;
    case ModificationRole:
      return static_cast<int>( mProjects.at( index.row() )->modification );
    case CheckoutRole:
      return static_cast<int>( mProjects.at( index.row() )->checkout );
    case StatusRole:
      return static_cast<int>( mProjects.at( index.row() )->status );
    case ErrorStatusRole:
      return static_cast<int>( mProjects.at( index.row() )->errorStatus );
    case ErrorStringRole:
      return mProjects.at( index.row() )->errorStatus == DownloadErrorStatus
               ? mProjects.at( index.row() )->packagingStatusString
             : mProjects.at( index.row() )->errorStatus == UploadErrorStatus
               ? mProjects.at( index.row() )->deltaFileUploadStatusString
               : QString();
    case PackagingStatusRole:
      return mProjects.at( index.row() )->packagingStatus;
    case PackagedLayerErrorsRole:
      return QVariant( mProjects.at( index.row() )->packagedLayerErrors );
    case DownloadProgressRole:
      return mProjects.at( index.row() )->downloadProgress;
    case UploadAttachmentsStatusRole:
      return mProjects.at( index.row() )->uploadAttachmentsStatus;
    case UploadAttachmentsCountRole:
      return mProjects.at( index.row() )->uploadAttachments.size() - mProjects.at( index.row() )->uploadAttachmentsFailed;
    case UploadDeltaProgressRole:
      return mProjects.at( index.row() )->uploadDeltaProgress;
    case UploadDeltaStatusRole:
      return mProjects.at( index.row() )->deltaFileUploadStatus;
    case UploadDeltaStatusStringRole:
      return mProjects.at( index.row() )->deltaFileUploadStatusString;
    case LocalDeltasCountRole:
      return mProjects.at( index.row() )->deltasCount;
    case LocalPathRole:
      return mProjects.at( index.row() )->localPath;
    case CanSyncRole:
      return canSyncProject( mProjects.at( index.row() )->id );
    case LastLocalExportedAtRole:
      return mProjects.at( index.row() )->lastLocalExportedAt;
    case LastLocalPushDeltasRole:
      return mProjects.at( index.row() )->lastLocalPushDeltas;
    case UserRoleRole:
      return mProjects.at( index.row() )->userRole;
    case DeltaListRole:
      return QVariant::fromValue<DeltaListModel *>( mProjects.at( index.row() )->deltaListModel );
  }

  return QVariant();
}

bool QFieldCloudProjectsModel::revertLocalChangesFromCurrentProject()
{
  CloudProject *project = findProject( mCurrentProjectId );

  if ( !project )
    return false;

  DeltaFileWrapper *deltaFileWrapper = mLayerObserver->deltaFileWrapper();

  if ( !deltaFileWrapper->toFile() )
    return false;

  if ( !deltaFileWrapper->applyReversed() )
  {
    QgsMessageLog::logMessage( QStringLiteral( "Failed to apply reversed" ) );
    return false;
  }

  project->modification ^= LocalModification;

  deltaFileWrapper->reset();
  deltaFileWrapper->resetId();

  if ( !deltaFileWrapper->toFile() )
    return false;

  return true;
}

bool QFieldCloudProjectsModel::discardLocalChangesFromCurrentProject()
{
  CloudProject *project = findProject( mCurrentProjectId );
  if ( !project )
    return false;

  DeltaFileWrapper *deltaFileWrapper = mLayerObserver->deltaFileWrapper();

  if ( !deltaFileWrapper->toFile() )
    QgsMessageLog::logMessage( QStringLiteral( "Failed to write deltas." ) );

  project->modification ^= LocalModification;

  deltaFileWrapper->reset();
  deltaFileWrapper->resetId();

  if ( !deltaFileWrapper->toFile() )
    return false;

  return true;
}

void QFieldCloudProjectsModel::setGpkgFlusher( QgsGpkgFlusher *flusher )
{
  if ( mGpkgFlusher == flusher )
    return;

  mGpkgFlusher = flusher;

  emit gpkgFlusherChanged();
}

bool QFieldCloudProjectsModel::deleteGpkgShmAndWal( const QStringList &gpkgFileNames )
{
  bool isSuccess = true;

  for ( const QString &fileName : gpkgFileNames )
  {
    QFile shmFile( QStringLiteral( "%1-shm" ).arg( fileName ) );
    if ( shmFile.exists() )
    {
      if ( !shmFile.remove() )
      {
        QgsMessageLog::logMessage( QStringLiteral( "Failed to remove -shm file '%1' " ).arg( shmFile.fileName() ) );
        isSuccess = false;
      }
    }

    QFile walFile( QStringLiteral( "%1-wal" ).arg( fileName ) );

    if ( walFile.exists() )
    {
      if ( !walFile.remove() )
      {
        QgsMessageLog::logMessage( QStringLiteral( "Failed to remove -wal file '%1' " ).arg( walFile.fileName() ) );
        isSuccess = false;
      }
    }
  }

  return isSuccess;
}

QStringList QFieldCloudProjectsModel::filterGpkgFileNames( const QStringList &fileNames ) const
{
  QStringList gpkgFileNames;

  for ( const QString &fileName : fileNames )
  {
    if ( fileName.endsWith( QStringLiteral( ".gpkg" ) ) )
    {
      gpkgFileNames.append( fileName );
    }
  }

  return gpkgFileNames;
}

QStringList QFieldCloudProjectsModel::projectFileNames( const QString &projectPath, const QStringList &fileNames ) const
{
  QStringList prefixedFileNames;

  for ( const QString &fileName : fileNames )
  {
    prefixedFileNames.append( QStringLiteral( "%1/%2" ).arg( projectPath, fileName ) );
  }

  return prefixedFileNames;
}

// --

QFieldCloudProjectsFilterModel::QFieldCloudProjectsFilterModel( QObject *parent )
  : QSortFilterProxyModel( parent )
{
}

void QFieldCloudProjectsFilterModel::setProjectsModel( QFieldCloudProjectsModel *projectsModel )
{
  if ( mSourceModel == projectsModel )
    return;

  mSourceModel = projectsModel;
  setSourceModel( mSourceModel );

  emit projectsModelChanged();
}

QFieldCloudProjectsModel *QFieldCloudProjectsFilterModel::projectsModel() const
{
  return mSourceModel;
}

void QFieldCloudProjectsFilterModel::setFilter( ProjectsFilter filter )
{
  if ( mFilter == filter )
    return;

  mFilter = filter;
  invalidateFilter();

  emit filterChanged();
}

QFieldCloudProjectsFilterModel::ProjectsFilter QFieldCloudProjectsFilterModel::filter() const
{
  return mFilter;
}

void QFieldCloudProjectsFilterModel::setShowLocalOnly( bool showLocalOnly )
{
  if ( mShowLocalOnly == showLocalOnly )
    return;

  mShowLocalOnly = showLocalOnly;
  invalidateFilter();

  emit showLocalOnlyChanged();
}

bool QFieldCloudProjectsFilterModel::showLocalOnly() const
{
  return mShowLocalOnly;
}

bool QFieldCloudProjectsFilterModel::filterAcceptsRow( int source_row, const QModelIndex &source_parent ) const
{
  if ( mShowLocalOnly && mSourceModel->data( mSourceModel->index( source_row, 0, source_parent ), QFieldCloudProjectsModel::LocalPathRole ).toString().isEmpty() )
  {
    return false;
  }

  bool ok = false;
  switch ( mFilter )
  {
    case PrivateProjects:
      // the list will include public "community" projects that are present locally so they can appear in the "My projects" list
      ok = mSourceModel->data( mSourceModel->index( source_row, 0, source_parent ), QFieldCloudProjectsModel::PrivateRole ).toBool() || !mSourceModel->data( mSourceModel->index( source_row, 0, source_parent ), QFieldCloudProjectsModel::LocalPathRole ).toString().isEmpty();
      break;
    case PublicProjects:
      ok = !mSourceModel->data( mSourceModel->index( source_row, 0, source_parent ), QFieldCloudProjectsModel::PrivateRole ).toBool();
      break;
  }
  return ok;
}
