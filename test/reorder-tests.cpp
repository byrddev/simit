#include "simit-test.h"

#include <iostream>

#include "graph.h"
#include "reorder.h"
#include "program.h"
#include "error.h"
#include "mesh.h"

using namespace std;
using namespace simit;

TEST(Program, reorderFemTet) {
  string dir(TEST_INPUT_DIR);
  string prefix=dir+"/program/fem/bar2k";
  string nodeFile = prefix + ".node";
  string eleFile = prefix + ".ele";
  size_t nSteps = 10;
  MeshVol mv;
  mv.loadTet(nodeFile.c_str(), eleFile.c_str());
  
  simit::Function m_precomputation;
  simit::Function m_timeStepper;

  Set m_verts;
  simit::Set m_tets(m_verts,m_verts,m_verts,m_verts);
  
  simit::FieldRef<simit_float,3>  x = m_verts.addField<simit_float,3>("x");
  simit::FieldRef<simit_float,3>  v = m_verts.addField<simit_float,3>("v");
  //external forces
  simit::FieldRef<simit_float,3> fe = m_verts.addField<simit_float,3>("fe");
  //constraintss
  simit::FieldRef<int>       c = m_verts.addField<int>("c");
  simit::FieldRef<simit_float>    m = m_verts.addField<simit_float>("m");
  
  simit::FieldRef<simit_float>    u = m_tets.addField<simit_float>("u");
  simit::FieldRef<simit_float>    l = m_tets.addField<simit_float>("l");
  simit::FieldRef<simit_float>    W = m_tets.addField<simit_float>("W");
  simit::FieldRef<simit_float,3,3>B = m_tets.addField<simit_float,3,3>("B");
  
  simit_float uval, lval;
  //Youngs modulus and poisson's ratio
  simit_float E = 5e3;
  simit_float nu = 0.45;
  uval = 0.5*E/nu;
  lval = E*nu/((1+nu)*(1-2*nu));
  
  //create nodes, intial velocity and constraints
  vector<simit::ElementRef> vertRefs;
  simit_float initV[3]={0.1, 0.0, 0.1};
  simit_float eps = 0.0001;

  for(unsigned int ii =0 ;ii<mv.v.size(); ii++){
    vertRefs.push_back(m_verts.add());
    simit::ElementRef p = vertRefs.back();
    x.set(p, {static_cast<simit_float>(mv.v[ii][0]),
              static_cast<simit_float>(mv.v[ii][1]),
              static_cast<simit_float>(mv.v[ii][2])});
    v.set(p, {initV[0], initV[1], initV[2]} );
    c.set(p,0);
    fe.set(p,{0.0,0.0,0.0});
    m.set(p, 0.0);
    if(mv.v[ii][1]<eps){
      c.set(p, 1);
      v.set(p, {0, 0, 0} );
    }
  }
  
  for(unsigned int ii =0 ;ii<mv.e.size(); ii++){
    simit::ElementRef t = m_tets.add(
      vertRefs[mv.e[ii][0]],vertRefs[mv.e[ii][1]],
      vertRefs[mv.e[ii][2]],vertRefs[mv.e[ii][3]]
    );
    u.set(t,uval);
    l.set(t,lval);    
  }
  
  std::string filename = std::string(TEST_INPUT_DIR) + "/" +
                         toLower(test_info_->test_case_name()) + "/" +
                         "femTet.sim";
  m_precomputation = loadFunction(filename, "initializeTet");

  m_precomputation.bind("verts", &m_verts);
  m_precomputation.bind("tets", &m_tets);
  m_precomputation.init();
  m_precomputation.unmapArgs();

  for (size_t i=0; i < nSteps; ++i) {
    m_precomputation.runSafe();
  }
  m_precomputation.mapArgs();  

  m_timeStepper = loadFunction(filename, "main");
  m_timeStepper.bind("verts", &m_verts);
  m_timeStepper.bind("tets", &m_tets);
  m_timeStepper.init();
  m_precomputation.unmapArgs();

  for (size_t i=0; i < nSteps; ++i) {
    m_timeStepper.runSafe();
  }
  m_timeStepper.mapArgs();

  // Check outputs
  SIMIT_ASSERT_FLOAT_EQ(0.010771915616785779,  x.get(vertRefs[100])(0));
  SIMIT_ASSERT_FLOAT_EQ(0.058853573999788439,  x.get(vertRefs[100])(1));
  SIMIT_ASSERT_FLOAT_EQ(0.030899457015375883,  x.get(vertRefs[100])(2));
  SIMIT_ASSERT_FLOAT_EQ(0.0028221631202928516, x.get(vertRefs[200])(0));
  SIMIT_ASSERT_FLOAT_EQ(0.017969982607667911,  x.get(vertRefs[200])(1));
  SIMIT_ASSERT_FLOAT_EQ(0.012885386063393013,  x.get(vertRefs[200])(2));
  SIMIT_ASSERT_FLOAT_EQ(0.02411959295647129,   x.get(vertRefs[300])(0));
  SIMIT_ASSERT_FLOAT_EQ(0.052036155669135678,  x.get(vertRefs[300])(1));
  SIMIT_ASSERT_FLOAT_EQ(0.030173075240629205,  x.get(vertRefs[300])(2));
}

