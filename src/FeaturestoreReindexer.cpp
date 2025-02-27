/*
 * This file is part of ePipe
 * Copyright (C) 2020, Logical Clocks AB. All rights reserved
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */
#include "FeaturestoreReindexer.h"
#include "FileProvenanceConstants.h"
#include "FsMutationsDataReader.h"
#include "tables/ProjectTable.h"
#include "tables/INodeTable.h"
#include "tables/DatasetTable.h"
#include "tables/XAttrTable.h"
#include "tables/FsMutationsLogTable.h"

FeaturestoreReindexer::FeaturestoreReindexer(const char* connection_string, const char* database_name,
        const char* meta_database_name, const char* hive_meta_database_name,
        const HttpClientConfig elastic_client_config, const std::string featurestore_index,
        int elastic_batch_size, int elastic_issue_time, int lru_cap)
        : ClusterConnectionBase(connection_string, database_name, meta_database_name, hive_meta_database_name),
        mFeaturestoreIndex(featurestore_index), mLRUCap(lru_cap) {
  mElasticSearch = new ProjectsElasticSearch(elastic_client_config, elastic_issue_time, elastic_batch_size, false, MConn());
}

void FeaturestoreReindexer::run() {
  ptime start = Utils::getCurrentTime();
  mElasticSearch->start();

  Ndb *metaConn = create_ndb_connection(mMetaDatabaseName);
  Ndb *conn = create_ndb_connection(mDatabaseName);

  ProjectTable projectsTable(mLRUCap);
  INodeTable inodesTable(mLRUCap);
  DatasetTable datasetsTable(mLRUCap);
  XAttrTable xAttrTable;

  int numXAttrs = 0;
  int nonExistentXAttrs = 0;
  int featurestoreDocs = 0;

  INodeRow projectsInode = inodesTable.getProjectsInode(conn);

  projectsTable.getAll(metaConn);
  while(projectsTable.next()) {
    ProjectRow project = projectsTable.currRow();
    INodeRow projectInode = inodesTable.get(conn, projectsInode.mId, project.mProjectName, projectsInode.mId);

    DatasetVec projectDatasets = datasetsTable.getByProjectId(metaConn, project.mId);
    for (DatasetVec::iterator datasetIt = projectDatasets.begin(); datasetIt != projectDatasets.end(); ++datasetIt) {
      DatasetRow dataset = *datasetIt;

      if(FileProvenanceConstants::isTrainingDataset(project.mProjectName, dataset.mDatasetName) ||
      FileProvenanceConstants::isFeaturestore(project.mProjectName, dataset.mDatasetName)) {
        INodeRow datasetInode = inodesTable.get(conn, projectInode.mId, dataset.mDatasetName, projectInode.mId);
        INodeVec artifactInodes = inodesTable.getByParentId(conn, datasetInode.mId, datasetInode.mId);
        for (INodeVec::iterator artifactInodesIt = artifactInodes.begin(); artifactInodesIt != artifactInodes.end(); ++artifactInodesIt) {
          INodeRow artifactInode = *artifactInodesIt;
          std::string docType = FileProvenanceConstants::getFeatureStoreArtifact(conn, inodesTable, project.mProjectName, dataset.mDatasetName, artifactInode.mName, datasetInode.mId, artifactInode.mParentId);
          if (docType != DONT_EXIST_STR()) {
            LOG_INFO("dataset:" << dataset.mDatasetName << " has " << docType << "s");
            //this is a featurestore doc - featuregroup, featureview or trainingdataset
            //The partition id is the parent id for all files and directories under project subtree
            boost::optional<std::pair<std::string, int>> nameParts = FileProvenanceConstants::splitNameVersion(artifactInode.mName);
            if (nameParts) {
              eBulk bulk;
              LOG_INFO("featurestore type:" << docType << " name:" << nameParts.get().first << " version:"
                      << std::to_string(nameParts.get().second));
              std::string featurestoreDoc = FSMutationsJSONBuilder::featurestoreDoc(mFeaturestoreIndex, docType,
                                                                                    artifactInode.mId,
                                                                                    nameParts.get().first,
                                                                                    nameParts.get().second,
                                                                                    dataset.mProjectId, project.mProjectName,
                                                                                    datasetInode.mId);
              bulk.push(Utils::getCurrentTime(), featurestoreDoc);
              featurestoreDocs++;

              if (artifactInode.has_xattrs()) {
                XAttrVec xattrs = xAttrTable.getByInodeId(conn, artifactInode.mId);
                for (XAttrVec::iterator xit = xattrs.begin(); xit != xattrs.end(); ++xit) {
                  XAttrRow xAttrRow = *xit;
                  LOG_INFO("xattr:" << xAttrRow.mName);
                  if (xAttrRow.mInodeId == artifactInode.mId) {
                    bulk.push(Utils::getCurrentTime(),
                              xAttrRow.to_upsert_json(mFeaturestoreIndex, FsOpType::XAttrUpdate));
                  } else {
                    LOG_WARN("XAttrs doesn't exists for [" << artifactInode.mId << "] - " << xAttrRow.to_string());
                    nonExistentXAttrs++;
                  }
                  numXAttrs++;
                }
              }
              mElasticSearch->addData(bulk);
            }
          }
        }
      }
    }
  }
  mElasticSearch->shutdown();
  mElasticSearch->waitToFinish();
  LOG_INFO((numXAttrs - nonExistentXAttrs) << " XAttrs added, " << nonExistentXAttrs << " doesn't exists");
  LOG_INFO((featurestoreDocs) << " documents were added to featurestore index ");
  LOG_INFO("Reindexing done in " << Utils::getTimeDiffInMilliseconds(start, Utils::getCurrentTime()) << " msec");
}
FeaturestoreReindexer::~FeaturestoreReindexer() {
}