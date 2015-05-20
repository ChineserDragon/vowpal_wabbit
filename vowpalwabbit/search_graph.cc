/*
Copyright (c) by respective owners including Yahoo!, Microsoft, and
individual contributors. All rights reserved.  Released under a BSD (revised)
license as described in the file LICENSE.
 */
#include "search_graph.h"
#include "vw.h"
#include "gd.h"

/*
example format:

ALL NODES
ALL EDGES
<blank>
ALL NODES
ALL EDGES
<blank>

and so on

node lines look like normal vw examples with unary features:

label:weight |n features
label:weight |n features
...

they are *implicitly* labeled starting at 1. (note the namespace
needn't be called n.) if weight is omitted it is assumed to be 1.0.

edge lines look like:

n1 n2 n3 ... |e features
n1 n2 n3 ... |e features
...

here, n1 n2 n3 are integer node ids, starting at one. technically
these are hyperedges, since they can touch more than two nodes. in the
canonical representation, there will just be n1 and n2.

the only thing that differentiates edges from nodes is that edges have
>1 input.
*/

namespace GraphTask {
  Search::search_task task = { "graph", run, initialize, finish, setup, takedown };

  struct task_data {
    // global data
    size_t num_loops;
    size_t K;  // number of labels, *NOT* including the +1 for 'unlabeled'
    bool   use_structure;
    bool   separate_learners;

    // for adding new features
    size_t mask; // all->reg.weight_mask
    size_t multiplier;   // all.wpp << all.reg.stride_shift
    size_t ss; // stride_shift
    size_t wpp;
    
    // per-example data
    uint32_t N;  // number of nodes
    uint32_t E;  // number of edges
    vector< vector<size_t> > adj;  // adj[n] is a vector of *edge example ids* that contain n
    vector<uint32_t> bfs;   // order of nodes to process
    vector<size_t>   pred;  // predictions
    example*cur_node;       // pointer to the current node for add_edge_features_fn
    float* neighbor_predictions;  // prediction on this neighbor for add_edge_features_fn
    weight* weight_vector;
    uint32_t* confusion_matrix;
    float* true_counts;
    float true_counts_total;
  };

  inline bool example_is_test(polylabel&l) { return l.cs.costs.size() == 0; }
  
  void initialize(Search::search& sch, size_t& num_actions, po::variables_map& vm) {
    task_data * D = new task_data();
    po::options_description sspan_opts("search graphtask options");
    sspan_opts.add_options()("search_graph_num_loops", po::value<size_t>(), "how many loops to run [def: 2]");
    sspan_opts.add_options()("search_graph_no_structure", "turn off edge features");
    sspan_opts.add_options()("search_graph_separate_learners", "use a different learner for each pass");
    sch.add_program_options(vm, sspan_opts);

    D->num_loops = 2;
    D->use_structure = true;
    if (vm.count("search_graph_num_loops"))      D->num_loops = vm["search_graph_num_loops"].as<size_t>();
    if (vm.count("search_graph_no_structure"))   D->use_structure = false;
    if (vm.count("search_graph_separate_learners")) D->separate_learners = true;

    if (D->num_loops <= 1) { D->num_loops = 1; D->separate_learners = false; }
    
    D->K = num_actions;
    D->neighbor_predictions = calloc_or_die<float>(D->K+1);

    D->confusion_matrix = calloc_or_die<uint32_t>( (D->K+1)*(D->K+1) );
    D->true_counts = calloc_or_die<float>(D->K+1);
    D->true_counts_total = (float)(D->K+1);
    for (size_t k=0; k<=D->K; k++) D->true_counts[k] = 1.;
    
    if (D->separate_learners) sch.set_num_learners(D->num_loops);
    
    sch.set_task_data<task_data>(D);
    sch.set_options( 0 ); // Search::AUTO_HAMMING_LOSS );
    sch.set_label_parser( COST_SENSITIVE::cs_label, example_is_test );
  }

  void finish(Search::search& sch) {
    task_data * D = sch.get_task_data<task_data>();
    free(D->neighbor_predictions);
    free(D->confusion_matrix);
    free(D->true_counts);
    delete D;
  }
  
  inline bool example_is_edge(example*e) { return e->l.cs.costs.size() > 1; }

  void run_bfs(task_data &D, vector<example*>& ec) {
    D.bfs.clear();
    vector<bool> touched;
    for (size_t n=0; n<D.N; n++) touched.push_back(false);

    touched[0] = true;
    D.bfs.push_back(0);

    size_t i = 0;
    while (D.bfs.size() < D.N) {
      while (i < D.bfs.size()) {
        uint32_t n = D.bfs[i];
        for (size_t id : D.adj[n])
          for (size_t j=0; j<ec[id]->l.cs.costs.size(); j++) {
            uint32_t m = ec[id]->l.cs.costs[j].class_index - 1;
            if (!touched[m]) {
              D.bfs.push_back(m);
              touched[m] = true;
            }
          }
        i++;
      }

      if (D.bfs.size() < D.N)
        // we finished a SCC, need to find another
        for (uint32_t n=0; n<D.N; n++)
          if (! touched[n]) {
            touched[n] = true;
            D.bfs.push_back(n);
            break;
          }
    }
  }
  
  void setup(Search::search& sch, vector<example*>& ec) {
    task_data& D = *sch.get_task_data<task_data>();

    D.mask = sch.get_vw_pointer_unsafe().reg.weight_mask;
    D.wpp  = sch.get_vw_pointer_unsafe().wpp;
    D.ss   = sch.get_vw_pointer_unsafe().reg.stride_shift;
    D.multiplier = D.wpp << D.ss;
    D.weight_vector = sch.get_vw_pointer_unsafe().reg.weight_vector;
    
    D.N = 0;
    D.E = 0;
    for (size_t i=0; i<ec.size(); i++)
      if (example_is_edge(ec[i]))
        D.E++;
      else { // it's a node!
        if (D.E > 0) { cerr << "error: got a node after getting edges!" << endl; throw exception(); }
        D.N++;
        if (ec[i]->l.cs.costs.size() > 0) {
          D.true_counts[ec[i]->l.cs.costs[0].class_index] += 1.;
          D.true_counts_total += 1.;
        }
      }

    if ((D.N == 0) && (D.E > 0)) { cerr << "error: got edges without any nodes (perhaps ring_size is too small?)!" << endl; throw exception(); }

    D.adj = vector<vector<size_t>>(D.N, vector<size_t>(0));

    for (size_t i=D.N; i<ec.size(); i++) {
      for (size_t n=0; n<ec[i]->l.cs.costs.size(); n++) {
        if (ec[i]->l.cs.costs[n].class_index > D.N) {
          cerr << "error: edge source points to too large of a node id: " << (ec[i]->l.cs.costs[n].class_index) << " > " << D.N << endl;
          throw exception();
        }
      }
      for (size_t n=0; n<ec[i]->l.cs.costs.size(); n++) {
        size_t nn = ec[i]->l.cs.costs[n].class_index - 1;
        if ((D.adj[nn].size() == 0) || (D.adj[nn][D.adj[nn].size()-1] != i)) // don't allow dups
          D.adj[nn].push_back(i);
      }
    }

    run_bfs(D, ec);

    D.pred.clear();
    for (size_t n=0; n<D.N; n++)
      D.pred.push_back( D.K+1 );
  }

  void takedown(Search::search& sch, vector<example*>& ec) {
    task_data& D = *sch.get_task_data<task_data>();
    D.bfs.clear();
    D.pred.clear();
    D.adj.clear();
  }

  void add_edge_features_group_fn(task_data&D, float fv, uint32_t fx) {
    example*node = D.cur_node;
    if (((fx / D.multiplier) * D.multiplier) != fx) { cerr << "eek"<<endl; throw exception();}
    //float fx2 = (fx & D.mask) / D.multiplier;
    //uint32_t fx2 = (fx >> D.ss) & D.mask;// / D.multiplier;
    uint32_t fx2 = fx / D.multiplier;
    for (size_t k=0; k<=D.K; k++) {
      if (D.neighbor_predictions[k] == 0.) continue;
      float fv2 = fv * D.neighbor_predictions[k];
      feature f = { fv2, (uint32_t)(( fx2 + 348919043 * k ) * D.multiplier) & (uint32_t)D.mask };
      node->atomics[neighbor_namespace].push_back(f);
      node->sum_feat_sq[neighbor_namespace] += f.x * f.x;
    }
    // TODO: audit
  }

  void add_edge_features_single_fn(task_data&D, float fv, uint32_t fx) {
    example*node = D.cur_node;
    if (((fx / D.multiplier) * D.multiplier) != fx) { cerr << "eek"<<endl; throw exception();}
    //uint32_t fx2 = (fx >> D.ss) & D.mask;// / D.multiplier;
    uint32_t fx2 = fx / D.multiplier;
    size_t k = (size_t) D.neighbor_predictions[0];
    feature f = { fv, (uint32_t)(( fx2 + 348919043 * k ) * D.multiplier) & (uint32_t)D.mask };
    node->atomics[neighbor_namespace].push_back(f);
    node->sum_feat_sq[neighbor_namespace] += f.x * f.x;
    // TODO: audit
  }
  
  void add_edge_features(Search::search&sch, task_data&D, uint32_t n, vector<example*>&ec) {
    D.cur_node = ec[n];

    for (size_t i : D.adj[n]) {
      for (size_t k=0; k<D.K+1; k++) D.neighbor_predictions[k] = 0.;

      float pred_total = 0.;
      uint32_t last_pred = 0;
      float one_over_K = 1. / max(1., (float)ec[i]->l.cs.costs.size() - 1.);
      if (D.use_structure)
        for (size_t j=0; j<ec[i]->l.cs.costs.size(); j++) {
          size_t m = ec[i]->l.cs.costs[j].class_index - 1;
          if (m == n) continue;
          D.neighbor_predictions[ D.pred[m]-1 ] += 1.; // one_over_K;
          pred_total += 1.;
          last_pred = D.pred[m]-1;
        }
      else {
        D.neighbor_predictions[0] += 1.; // one_over_K;
        pred_total += 1.;
        last_pred = 0;
      }

      if (pred_total == 0.) continue;
      //for (size_t k=0; k<D.K+1; k++) D.neighbor_predictions[k] /= pred_total;
      example&edge = *ec[i];
      if (pred_total <= 1.) {  // single edge
        D.neighbor_predictions[0] = (float)last_pred;
        GD::foreach_feature<task_data,uint32_t,add_edge_features_single_fn>(sch.get_vw_pointer_unsafe(), edge, D);
      } else // lots of edges
        GD::foreach_feature<task_data,uint32_t,add_edge_features_group_fn>(sch.get_vw_pointer_unsafe(), edge, D);
    }
    ec[n]->indices.push_back(neighbor_namespace);
    ec[n]->total_sum_feat_sq += ec[n]->sum_feat_sq[neighbor_namespace];
    ec[n]->num_features += ec[n]->atomics[neighbor_namespace].size();

    vw& all = sch.get_vw_pointer_unsafe();
    for (vector<string>::iterator i = all.pairs.begin(); i != all.pairs.end();i++) {
      int i0 = (int)(*i)[0];
      int i1 = (int)(*i)[1];
      if ((i0 == neighbor_namespace) || (i1 == neighbor_namespace)) {
        ec[n]->num_features      += ec[n]->atomics[i0].size() * ec[n]->atomics[i1].size();
        ec[n]->total_sum_feat_sq += ec[n]->sum_feat_sq[i0]*ec[n]->sum_feat_sq[i1];
      }
    }
    
  }
  
  void del_edge_features(task_data&D, uint32_t n, vector<example*>&ec) {
    ec[n]->indices.pop();
    ec[n]->total_sum_feat_sq -= ec[n]->sum_feat_sq[neighbor_namespace];
    ec[n]->num_features -= ec[n]->atomics[neighbor_namespace].size();
    ec[n]->atomics[neighbor_namespace].erase();
    ec[n]->sum_feat_sq[neighbor_namespace] = 0.;
  }

#define IDX(i,j) ( (i) * (D.K+1) + j )

  float macro_f(task_data& D) {
    float total_f1 = 0.;
    float count_f1 = 0.;
    for (size_t k=1; k<=D.K; k++) {
      float trueC = 0.;
      float predC = 0.;
      for (size_t j=1; j<=D.K; j++) {
        trueC += (float)D.confusion_matrix[ IDX(k,j) ];
        predC += (float)D.confusion_matrix[ IDX(j,k) ];
      }
      if (trueC == 0) continue;
      float correctC = D.confusion_matrix[ IDX(k,k) ];
      count_f1++;
      if (correctC > 0) {
        float pre = correctC / predC;
        float rec = correctC / trueC;
        total_f1 += 2 * pre * rec / (pre + rec);
      }
    }
    return total_f1 / count_f1;
  }
  
  void run(Search::search& sch, vector<example*>& ec) {
    task_data& D = *sch.get_task_data<task_data>();
    size_t K_squared = (D.K+1)*(D.K+1);
    
    memset(D.confusion_matrix, 0, K_squared * sizeof(uint32_t));
    
    for (size_t n=0; n<D.N; n++) D.pred[n] = D.K+1;
    
    for (size_t loop=0; loop<D.num_loops; loop++) {
      int start = 0; int end = D.N; int step = 1;
      if (loop % 2 == 1) { start = D.N-1; end=-1; step = -1; } // go inward on odd loops
      for (int n_id = start; n_id != end; n_id += step) {
        uint32_t n = D.bfs[n_id];
        uint32_t k = (ec[n]->l.cs.costs.size() > 0) ? ec[n]->l.cs.costs[0].class_index : 0;

        bool add_features = /* D.use_structure && */ sch.predictNeedsExample();
        //add_features = false;

        if (add_features) add_edge_features(sch, D, n, ec);
        Search::predictor P = Search::predictor(sch, n+1);
        P.set_input(*ec[n]);
        if (k > 0) {
          float min_count = 1e12;
          for (size_t k2=1; k2<=D.K; k2++)
            min_count = min(min_count, D.true_counts[k2]);
          //float w = min_count / D.true_counts[k];
          float w = D.true_counts_total / D.true_counts[k] / (float)(D.K);
          //P.set_weight( sqrt(w) );
          //cerr << "w = " << D.true_counts_total / D.true_counts[k] / (float)(D.K) << endl;
          //P.set_weight( D.true_counts_total / D.true_counts[k] / (float)(D.K) );
        }
        if (D.separate_learners) P.set_learner_id(loop);
        if (k > 0) // for test examples
          P.set_oracle(k);
        // add all the conditioning
        for (size_t i=0; i<D.adj[n].size(); i++) {
          for (size_t j=0; j<ec[i]->l.cs.costs.size(); j++) {
            uint32_t m = ec[i]->l.cs.costs[j].class_index - 1;
            if (m == n) continue;
            P.add_condition(m+1, 'e');
          }
        }

        // make the prediction
        D.pred[n] = P.predict();
        
        if (add_features) del_edge_features(D, n, ec);
      }
    }

    for (uint32_t n=0; n<D.N; n++)
      D.confusion_matrix[ IDX( ec[n]->l.cs.costs[0].class_index, D.pred[n] ) ] ++;
    sch.loss( 1. - macro_f(D) );
    
    if (sch.output().good())
      for (uint32_t n=0; n<D.N; n++)
        sch.output() << D.pred[n] << ' ';
  }
}

// TODO: in graph data, ef0= should be ef0:
