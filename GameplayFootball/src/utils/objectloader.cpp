// written by bastiaan konings schuiling 2008 - 2014
// this work is public domain. the code is undocumented, scruffy, untested, and should generally not be used for anything important.
// i do not offer support, so don't ask. to be used for inspiration :)

#include "objectloader.hpp"

#include "base/utils.hpp"
#include "types/resource.hpp"
#include "scene/resources/geometrydata.hpp"
#include "scene/objects/geometry.hpp"
#include "scene/objects/light.hpp"
#include "scene/objects/joint.hpp"
#include "managers/resourcemanagerpool.hpp"
#include "scene/objectfactory.hpp"

#include <iostream>

namespace blunted {

  ObjectLoader::ObjectLoader() {
  }

  ObjectLoader::~ObjectLoader() {
  }

  boost::intrusive_ptr<Node> ObjectLoader::LoadLevel(boost::shared_ptr<Scene3D> scene3D, const std::string &filename) const {

    XMLLoader loader;
    XMLTree levelTree = loader.LoadFile(filename);

    boost::intrusive_ptr<Node> levelNode(new Node("levelnode: " + filename));

    map_XMLTree::iterator iter = levelTree.children.begin()->second.children.begin();
    while (iter != levelTree.children.begin()->second.children.end()) {
      if ((*iter).first == "object") {
        //printf("object found \n");

        // filename & position
        std::string filename;
        Vector3 position;
        Quaternion rotation;
        radian angle = 0;

        map_XMLTree::iterator objectIter = (*iter).second.children.begin();
        while (objectIter != (*iter).second.children.end()) {
          if (objectIter->first == "filename") {
            //printf("file: %s\n", objectIter->second.value.c_str());
            filename = objectIter->second.value;
          }
          if (objectIter->first == "position") {
            //printf("position: %s\n", objectIter->second.value.c_str());
            position = GetVectorFromString(objectIter->second.value);
          }
          if (objectIter->first == "rotation") {
            rotation = GetQuaternionFromString(objectIter->second.value);
          }

          objectIter++;
        }

        boost::intrusive_ptr<Node> objNode = LoadObject(scene3D, "media/" + filename);
        objNode->SetPosition(position);
        objNode->SetRotation(rotation);

        levelNode->AddNode(objNode);
      }

      iter++;
    }

    return levelNode;
  }

  boost::intrusive_ptr<Node> ObjectLoader::LoadObject(boost::shared_ptr<Scene3D> scene3D, const std::string &filename, const Vector3 &offset) const {
    std::cout << "[ObjectLoader::LoadObject] loading XML: " << filename << std::endl;

    XMLLoader loader;
    const XMLTree objectTree = loader.LoadFile(filename);
    std::cout << "[ObjectLoader::LoadObject] XML loaded, children=" << objectTree.children.size() << std::endl;

    if (objectTree.children.empty()) {
      std::cout << "[ObjectLoader::LoadObject] ERROR: empty XML tree!" << std::endl;
      return boost::intrusive_ptr<Node>();
    }

    map_XMLTree::const_iterator firstChild = objectTree.children.begin();
    std::cout << "[ObjectLoader::LoadObject] first child tag=" << firstChild->first << std::endl;
    // printf("\n\n");
    // loader.PrintTree(objectTree);
    // printf("\n\n");
    boost::intrusive_ptr<Node> result = LoadObjectImpl(scene3D, filename, firstChild->second, offset);
    std::cout << "[ObjectLoader::LoadObject] LoadObjectImpl done" << std::endl;
    // printf("\n\n");
    // result->PrintTree();
    // printf("\n\n");
    return result;
  }

  boost::intrusive_ptr<Node> ObjectLoader::LoadObjectImpl(boost::shared_ptr<Scene3D> scene3D, const std::string &nodename, const XMLTree &objectTree, const Vector3 &offset) const {

    //printf("processing node: %s\n", nodename.c_str());
    boost::intrusive_ptr<Node> objNode(new Node("objectnode: " + nodename));

    std::string dirpart = nodename.substr(0, nodename.find_last_of('/') + 1);

    Vector3 position;
    Quaternion rotation;

    map_XMLTree::const_iterator objectIter = objectTree.children.begin();
    while (objectIter != objectTree.children.end()) {

      e_ObjectType objectType;
      std::string objectName;
      Properties properties;
      e_LocalMode localMode = e_LocalMode_Relative;

      //printf("loading %s:%s\n", nodename.c_str(), (*objectIter).first.c_str());


      // NODE (recurse)

      if (objectIter->first == "node") {
        objNode->AddNode(LoadObjectImpl(scene3D, dirpart, objectIter->second, offset));
      }

      else if (objectIter->first == "name") {
        objectName = objectIter->second.value;
        //printf("node name: %s\n", objectIter->second.value.c_str());
        objNode->SetName(objectName);
      }

      else if (objectIter->first == "position") {
        position = GetVectorFromString(objectIter->second.value) + offset;
        objNode->SetPosition(position);
      }

      else if (objectIter->first == "rotation") {
        rotation = GetQuaternionFromString(objectIter->second.value);
        objNode->SetRotation(rotation);
      }


      // GEOMETRY

      else if (objectIter->first == "geometry") {
        std::cout << "[LoadObjectImpl] processing geometry node" << std::endl;

        objectType = e_ObjectType_Geometry;

        std::string aseFilename;
        Vector3 position;
        Quaternion rotation;

        //printf("loading geom\n");

        map_XMLTree::const_iterator iter = objectIter->second.children.begin();
        while (iter != objectIter->second.children.end()) {

          if (iter->first == "filename") {
            aseFilename = iter->second.value;
            std::cout << "[LoadObjectImpl] geometry filename=" << aseFilename << std::endl;
          }
          if (iter->first == "name") {
            objectName = iter->second.value;
            //printf("geom name: %s\n", objectName.c_str());
          }
          if (iter->first == "position") {
            position = GetVectorFromString(iter->second.value) + offset;
          }
          if (iter->first == "rotation") {
            rotation = GetQuaternionFromString(iter->second.value);
          }
          if (iter->first == "properties") {
            InterpretProperties(iter->second.children, properties);
          }
          if (iter->first == "localmode") {
            localMode = InterpretLocalMode(iter->second.value);
          }

          iter++;
        }
        std::cout << "[LoadObjectImpl] Fetching geometry: " << (dirpart + aseFilename) << std::endl;
        boost::intrusive_ptr < Resource<GeometryData> > geometry = ResourceManagerPool::GetInstance().GetManager<GeometryData>(e_ResourceType_GeometryData)->Fetch(dirpart + aseFilename, true);
        std::cout << "[LoadObjectImpl] geometry fetched" << std::endl;
        boost::intrusive_ptr<Geometry> object = static_pointer_cast<Geometry>(ObjectFactory::GetInstance().CreateObject(objectName, objectType));
        std::cout << "[LoadObjectImpl] object created" << std::endl;
        if (properties.GetBool("dynamic")) geometry->GetResource()->SetDynamic(true);

        object->SetProperties(properties);
        std::cout << "[LoadObjectImpl] before CreateSystemObjects" << std::endl;
        scene3D->CreateSystemObjects(object);
        std::cout << "[LoadObjectImpl] after CreateSystemObjects" << std::endl;
        object->SetLocalMode(localMode);
        object->SetPosition(position);
        object->SetRotation(rotation);
        object->SetGeometryData(geometry);
        objNode->AddObject(object);
        std::cout << "[LoadObjectImpl] geometry object added to node" << std::endl;
      }


      // LIGHT

      else if (objectIter->first == "light") {

        objectType = e_ObjectType_Light;

        Vector3 position;

        map_XMLTree::const_iterator iter = objectIter->second.children.begin();
        while (iter != objectIter->second.children.end()) {

          if (iter->first == "name") {
            objectName = iter->second.value;
          }
          if (iter->first == "position") {
            position = GetVectorFromString(iter->second.value);
          }
          if (iter->first == "properties") {
            InterpretProperties(iter->second.children, properties);
          }
          if (iter->first == "localmode") {
            localMode = InterpretLocalMode(iter->second.value);
            //if (localMode == e_localMode_
          }

          iter++;
        }

        boost::intrusive_ptr<Light> object = static_pointer_cast<Light>(ObjectFactory::GetInstance().CreateObject(objectName, objectType));

        //object->SetProperties(properties);
        scene3D->CreateSystemObjects(object);
        object->SetLocalMode(localMode);
        object->SetColor(GetVectorFromString(properties.Get("color")));
        object->SetRadius(properties.GetReal("radius"));
        if (properties.Get("type") == "directional") {
          object->SetType(e_LightType_Directional);
        } else {
          object->SetType(e_LightType_Point);
        }
        object->SetShadow(properties.GetBool("shadow"));
        object->SetPosition(position);
        objNode->AddObject(object);
      }


      // JOINT waaaah smoke em

      else if (objectIter->first == "joint") {

        objectType = e_ObjectType_Joint;

        Vector3 anchor(0, 0, 0);
        Vector3 axis_1(0, 0, 0);
        Vector3 axis_2(0, 0, 0);
        std::string target_1 = "";
        std::string target_2 = "";

        map_XMLTree::const_iterator iter = objectIter->second.children.begin();
        while (iter != objectIter->second.children.end()) {

          if (iter->first == "name") {
            objectName = iter->second.value;
          }
          if (iter->first == "anchor") {
            anchor = GetVectorFromString(iter->second.value) + offset;
          }
          if (iter->first == "axis_1") {
            axis_1 = GetVectorFromString(iter->second.value);
          }
          if (iter->first == "axis_2") {
            axis_2 = GetVectorFromString(iter->second.value);
          }
          if (iter->first == "target_1") {
            target_1 = iter->second.value;
          }
          if (iter->first == "target_2") {
            target_2 = iter->second.value;
          }
          if (iter->first == "properties") {
            InterpretProperties(iter->second.children, properties);
          }

          iter++;
        }

        boost::intrusive_ptr<Joint> object = static_pointer_cast<Joint>(ObjectFactory::GetInstance().CreateObject(objectName, objectType));
        object->SetProperties(properties);
        scene3D->CreateSystemObjects(object);
        objNode->AddObject(object);

        boost::intrusive_ptr<Geometry> target_1_object;
        boost::intrusive_ptr<Geometry> target_2_object;
        if (target_1 != "") target_1_object = static_pointer_cast<Geometry>(objNode->GetObject(target_1));
        if (target_2 != "") target_2_object = static_pointer_cast<Geometry>(objNode->GetObject(target_2));
        object->Connect(target_1_object, target_2_object, anchor, axis_1, axis_2);
      }

      objectIter++;
    }

    return objNode;
  }

  void ObjectLoader::InterpretProperties(const map_XMLTree &tree, Properties &properties) const {
    map_XMLTree::const_iterator propIter = tree.begin();
    while (propIter != tree.end()) {
      properties.Set(propIter->first.c_str(), propIter->second.value);
      //printf("%s %s\n", propIter->first.c_str(), propIter->second.value.c_str());
      propIter++;
    }
  }

  e_LocalMode ObjectLoader::InterpretLocalMode(const std::string &value) const {
    if (value.compare("absolute") == 0) {
      return e_LocalMode_Absolute;
    } else {
      return e_LocalMode_Relative;
    }
  }

}
