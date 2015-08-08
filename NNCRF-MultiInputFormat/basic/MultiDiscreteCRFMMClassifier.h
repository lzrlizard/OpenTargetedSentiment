/*
 * MultiDiscreteCRFMMClassifier.h
 *
 *  Created on: Mar 18, 2015
 *      Author: mszhang
 */

#ifndef SRC_MultiDiscreteCRFMMClassifier_H_
#define SRC_MultiDiscreteCRFMMClassifier_H_

#include <iostream>
#include <armadillo>
#include <assert.h>
#include "Example.h"
#include "Feature.h"
#include "Metric.h"
#include "NRMat.h"
#include "MyLib.h"
#include "tensor.h"

#include "SparseUniHidderLayer.h"
#include "Utiltensor.h"

using namespace nr;
using namespace std;
using namespace mshadow;
using namespace mshadow::expr;
using namespace mshadow::utils;

//A native neural network classfier using only word embeddings
template<typename xpu>
class MultiDiscreteCRFMMClassifier {
public:
  MultiDiscreteCRFMMClassifier() {
    _dropOut = 0.5;
  }
  ~MultiDiscreteCRFMMClassifier() {

  }

public:
  int _label1Size, _label1_o;
  int _label2Size, _label2_o;
  int _linearfeatSize;

  double _dropOut;
  Metric _eval1, _eval2;

  Tensor<xpu, 2, double> _tagBigram1;
  Tensor<xpu, 2, double> _grad_tagBigram1;
  Tensor<xpu, 2, double> _eg2_tagBigram1;

  Tensor<xpu, 2, double> _tagBigram2;
  Tensor<xpu, 2, double> _grad_tagBigram2;
  Tensor<xpu, 2, double> _eg2_tagBigram2;

  SparseUniHidderLayer<xpu> _layer_linear1;
  SparseUniHidderLayer<xpu> _layer_linear2;

public:

  inline void init(int label1Size, int label2Size, int linearfeatSize) {
    _label1Size = label1Size;
    _label2Size = label2Size;
    _linearfeatSize = linearfeatSize;

    _tagBigram1 = NewTensor<xpu>(Shape2(_label1Size, _label1Size), 0.0);
    _grad_tagBigram1 = NewTensor<xpu>(Shape2(_label1Size, _label1Size), 0.0);
    _eg2_tagBigram1 = NewTensor<xpu>(Shape2(_label1Size, _label1Size), 0.0);

    Random<xpu, double> rnd(100);
    rnd.SampleUniform(&_tagBigram1, -0.1, 0.1);

    _layer_linear1.initial(_label1Size, _linearfeatSize, false, 4, 2);
    _eval1.reset();

    _tagBigram2 = NewTensor<xpu>(Shape2(_label2Size, _label2Size), 0.0);
    _grad_tagBigram2 = NewTensor<xpu>(Shape2(_label2Size, _label2Size), 0.0);
    _eg2_tagBigram2 = NewTensor<xpu>(Shape2(_label2Size, _label2Size), 0.0);

    rnd.SampleUniform(&_tagBigram2, -0.1, 0.1);

    _layer_linear2.initial(_label2Size, _linearfeatSize, false, 5, 2);
    _eval2.reset();


  }

  inline void release() {

    _layer_linear2.release();

    FreeSpace(&_tagBigram2);
    FreeSpace(&_grad_tagBigram2);
    FreeSpace(&_eg2_tagBigram2);

    _layer_linear1.release();

    FreeSpace(&_tagBigram1);
    FreeSpace(&_grad_tagBigram1);
    FreeSpace(&_eg2_tagBigram1);

  }

  inline double process(const vector<Example>& examples, int iter) {
    _eval1.reset();
    _eval2.reset();

    int example_num = examples.size();
    double cost = 0.0;
    int offset = 0;

    for (int count = 0; count < example_num; count++) {
      const Example& example = examples[count];

      int seq_size = example.m_features.size();
      Tensor<xpu, 2, double> output1[seq_size], output1Loss[seq_size];
      Tensor<xpu, 2, double> output2[seq_size], output2Loss[seq_size];

      //initialize
      for (int idx = 0; idx < seq_size; idx++) {
        output1[idx] = NewTensor<xpu>(Shape2(1, _label1Size), 0.0);
        output1Loss[idx] = NewTensor<xpu>(Shape2(1, _label1Size), 0.0);
        output2[idx] = NewTensor<xpu>(Shape2(1, _label2Size), 0.0);
        output2Loss[idx] = NewTensor<xpu>(Shape2(1, _label2Size), 0.0);
      }

      //forward propagation
      vector<vector<int> > linear_features(seq_size);
      for (int idx = 0; idx < seq_size; idx++) {
        const Feature& feature = example.m_features[idx];
        srand(iter * example_num + count * seq_size + idx);
        linear_features[idx].clear();
        for (int idy = 0; idy < feature.linear_features.size(); idy++) {
          if (1.0 * rand() / RAND_MAX >= _dropOut) {
            linear_features[idx].push_back(feature.linear_features[idy]);
          }
        }
        _layer_linear1.ComputeForwardScore(linear_features[idx], output1[idx]);
        _layer_linear2.ComputeForwardScore(linear_features[idx], output2[idx]);
      }

      // viterbi algorithm
      NRVec<int> goldlabel1s(seq_size);
      goldlabel1s = -1;
      for (int idx = 0; idx < seq_size; idx++) {
        for (int i = 0; i < _label1Size; ++i) {
          if (example.m_label1s[idx][i] == 1) {
            goldlabel1s[idx] = i;
          }
        }
      }

      NRMat<double> maxscore1s(seq_size, _label1Size);
      NRMat<int> maxlastlabel1s(seq_size, _label1Size);
      double gold1Score = 0.0;
      for (int idx = 0; idx < seq_size; idx++) {
        if (idx == 0)
          gold1Score = output1[idx][0][goldlabel1s[idx]];
        else
          gold1Score += output1[idx][0][goldlabel1s[idx]] + _tagBigram1[goldlabel1s[idx - 1]][goldlabel1s[idx]];
        double delta = 1.0;
        for (int i = 0; i < _label1Size; ++i) {
          // can be changed with probabilities in future work
          if (idx == 0) {
            maxscore1s[idx][i] = output1[idx][0][i];
            if (goldlabel1s[idx] != i)
              maxscore1s[idx][i] = maxscore1s[idx][i] + delta;
            maxlastlabel1s[idx][i] = -1;
          } else {
            int maxlastlabel = 0;
            double maxscore = _tagBigram1[0][i] + output1[idx][0][i] + maxscore1s[idx - 1][0];
            for (int j = 1; j < _label1Size; ++j) {
              double curscore = _tagBigram1[j][i] + output1[idx][0][i] + maxscore1s[idx - 1][j];
              if (curscore > maxscore) {
                maxlastlabel = j;
                maxscore = curscore;
              }
            }
            maxscore1s[idx][i] = maxscore;
            if (goldlabel1s[idx] != i)
              maxscore1s[idx][i] = maxscore1s[idx][i] + delta;
            maxlastlabel1s[idx][i] = maxlastlabel;

          }
        }
      }

      NRVec<int> optLabel1s(seq_size);
      optLabel1s = 0;
      double max1Score = maxscore1s[seq_size - 1][0];
      for (int i = 1; i < _label1Size; ++i) {
        if (maxscore1s[seq_size - 1][i] > max1Score) {
          max1Score = maxscore1s[seq_size - 1][i];
          optLabel1s[seq_size - 1] = i;
        }
      }

      for (int idx = seq_size - 2; idx >= 0; idx--) {
        optLabel1s[idx] = maxlastlabel1s[idx + 1][optLabel1s[idx + 1]];
      }

      bool bCorrect = true;
      for (int idx = 0; idx < seq_size; idx++) {
        if (goldlabel1s[idx] == -1)
          continue;
        _eval1.overall_label_count++;
        if (optLabel1s[idx] == goldlabel1s[idx]) {
          _eval1.correct_label_count++;
        } else {
          bCorrect = false;
        }
      }

      double cur1cost = bCorrect ? 0.0 : max1Score - gold1Score;
      cur1cost = cur1cost / example_num;

      for (int idx = 0; idx < seq_size; idx++) {
        if (goldlabel1s[idx] == -1)
          continue;
        if (optLabel1s[idx] != goldlabel1s[idx]) {
          output1Loss[idx][0][optLabel1s[idx]] = cur1cost;
          output1Loss[idx][0][goldlabel1s[idx]] = -cur1cost;
          cost += cur1cost;
        }
        if (idx > 0 && goldlabel1s[idx - 1] >= 0) {
          _grad_tagBigram1[optLabel1s[idx - 1]][optLabel1s[idx]] += cur1cost;
          _grad_tagBigram1[goldlabel1s[idx - 1]][goldlabel1s[idx]] -= cur1cost;
        }
      }

      // viterbi algorithm
      NRVec<int> goldlabel2s(seq_size);
      goldlabel2s = -1;
      for (int idx = 0; idx < seq_size; idx++) {
        for (int i = 0; i < _label2Size; ++i) {
          if (example.m_label2s[idx][i] == 1) {
            goldlabel2s[idx] = i;
          }
        }
      }

      NRMat<double> maxscore2s(seq_size, _label2Size);
      NRMat<int> maxlastlabel2s(seq_size, _label2Size);
      double gold2Score = 0.0;
      for (int idx = 0; idx < seq_size; idx++) {
        if (idx == 0)
          gold2Score = output2[idx][0][goldlabel2s[idx]];
        else
          gold2Score += output2[idx][0][goldlabel2s[idx]] + _tagBigram2[goldlabel2s[idx - 1]][goldlabel2s[idx]];
        double delta = 1.0;
        for (int i = 0; i < _label2Size; ++i) {
          // can be changed with probabilities in future work
          if (idx == 0) {
            maxscore2s[idx][i] = output2[idx][0][i];
            if (goldlabel2s[idx] != i)
              maxscore2s[idx][i] = maxscore2s[idx][i] + delta;
            maxlastlabel2s[idx][i] = -1;
          } else {
            int maxlastlabel = 0;
            double maxscore = _tagBigram2[0][i] + output2[idx][0][i] + maxscore2s[idx - 1][0];
            for (int j = 1; j < _label2Size; ++j) {
              double curscore = _tagBigram2[j][i] + output2[idx][0][i] + maxscore2s[idx - 1][j];
              if (curscore > maxscore) {
                maxlastlabel = j;
                maxscore = curscore;
              }
            }
            maxscore2s[idx][i] = maxscore;
            if (goldlabel2s[idx] != i)
              maxscore2s[idx][i] = maxscore2s[idx][i] + delta;
            maxlastlabel2s[idx][i] = maxlastlabel;
          }
          if (optLabel1s[idx] == _label1_o && i != _label2_o)
            maxscore2s[idx][i] = -1e+20;
        }

      }

      NRVec<int> optLabel2s(seq_size);
      optLabel2s = 0;
      double max2Score = maxscore2s[seq_size - 1][0];
      for (int i = 1; i < _label2Size; ++i) {
        if (maxscore2s[seq_size - 1][i] > max2Score) {
          max2Score = maxscore2s[seq_size - 1][i];
          optLabel2s[seq_size - 1] = i;
        }
      }

      for (int idx = seq_size - 2; idx >= 0; idx--) {
        optLabel2s[idx] = maxlastlabel2s[idx + 1][optLabel2s[idx + 1]];
      }

      bCorrect = true;
      for (int idx = 0; idx < seq_size; idx++) {
        if (goldlabel2s[idx] == -1)
          continue;
        _eval2.overall_label_count++;
        if (optLabel2s[idx] == goldlabel2s[idx]) {
          _eval2.correct_label_count++;
        } else {
          bCorrect = false;
        }
      }

      double cur2cost = bCorrect ? 0.0 : max2Score - gold2Score;

      for (int idx = 0; idx < seq_size; idx++) {
        if (goldlabel2s[idx] == -1)
          continue;
        if (optLabel2s[idx] != goldlabel2s[idx]) {
          output2Loss[idx][0][optLabel2s[idx]] = cur2cost;
          output2Loss[idx][0][goldlabel2s[idx]] = -cur2cost;
          cost += cur2cost;
        }
        if (idx > 0 && goldlabel2s[idx - 1] >= 0) {
          _grad_tagBigram2[optLabel2s[idx - 1]][optLabel2s[idx]] += cur2cost;
          _grad_tagBigram2[goldlabel2s[idx - 1]][goldlabel2s[idx]] -= cur2cost;
        }
      }

      // loss backward propagation
      for (int idx = 0; idx < seq_size; idx++) {
        _layer_linear1.ComputeBackwardLoss(linear_features[idx], output1[idx], output1Loss[idx]);
        _layer_linear2.ComputeBackwardLoss(linear_features[idx], output2[idx], output2Loss[idx]);
      }

      //release
      for (int idx = 0; idx < seq_size; idx++) {
        FreeSpace(&(output1[idx]));
        FreeSpace(&(output1Loss[idx]));
        FreeSpace(&(output2[idx]));
        FreeSpace(&(output2Loss[idx]));
      }
    }

    if (_eval1.getAccuracy() < 0 || _eval2.getAccuracy() < 0) {
      std::cout << "strange" << std::endl;
    }

    return cost;
  }

  void predict(const vector<Feature>& features, vector<int>& result1s, vector<int>& result2s) {
    int seq_size = features.size();
    Tensor<xpu, 2, double> output1[seq_size];
    Tensor<xpu, 2, double> output2[seq_size];

    //initialize
    for (int idx = 0; idx < seq_size; idx++) {
      output1[idx] = NewTensor<xpu>(Shape2(1, _label1Size), 0.0);
      output2[idx] = NewTensor<xpu>(Shape2(1, _label2Size), 0.0);
    }

    //forward propagation
    for (int idx = 0; idx < seq_size; idx++) {
      const Feature& feature = features[idx];
      _layer_linear1.ComputeForwardScore(feature.linear_features, output1[idx]);
      _layer_linear2.ComputeForwardScore(feature.linear_features, output2[idx]);
    }
    //end gru

    // viterbi algorithm
    NRMat<double> maxscore1s(seq_size, _label1Size);
    NRMat<int> maxlastlabel1s(seq_size, _label1Size);

    for (int idx = 0; idx < seq_size; idx++) {
      for (int i = 0; i < _label1Size; ++i) {
        // can be changed with probabilities in future work
        if (idx == 0) {
          maxscore1s[idx][i] = output1[idx][0][i];
          maxlastlabel1s[idx][i] = -1;
        } else {
          int maxlastlabel = 0;
          double maxscore = _tagBigram1[0][i] + output1[idx][0][i] + maxscore1s[idx - 1][0];
          for (int j = 1; j < _label1Size; ++j) {
            double curscore = _tagBigram1[j][i] + output1[idx][0][i] + maxscore1s[idx - 1][j];
            if (curscore > maxscore) {
              maxlastlabel = j;
              maxscore = curscore;
            }
          }
          maxscore1s[idx][i] = maxscore;
          maxlastlabel1s[idx][i] = maxlastlabel;
        }
      }
    }

    result1s.resize(seq_size);
    double maxFinalScore = maxscore1s[seq_size - 1][0];
    result1s[seq_size - 1] = 0;
    for (int i = 1; i < _label1Size; ++i) {
      if (maxscore1s[seq_size - 1][i] > maxFinalScore) {
        maxFinalScore = maxscore1s[seq_size - 1][i];
        result1s[seq_size - 1] = i;
      }
    }

    for (int idx = seq_size - 2; idx >= 0; idx--) {
      result1s[idx] = maxlastlabel1s[idx + 1][result1s[idx + 1]];
    }

    // viterbi algorithm
    NRMat<double> maxscore2s(seq_size, _label2Size);
    NRMat<int> maxlastlabel2s(seq_size, _label2Size);

    for (int idx = 0; idx < seq_size; idx++) {
      for (int i = 0; i < _label2Size; ++i) {
        // can be changed with probabilities in future work
        if (idx == 0) {
          maxscore2s[idx][i] = output2[idx][0][i];
          maxlastlabel2s[idx][i] = -1;
        } else {
          int maxlastlabel = 0;
          double maxscore = _tagBigram2[0][i] + output2[idx][0][i] + maxscore2s[idx - 1][0];
          for (int j = 1; j < _label2Size; ++j) {
            double curscore = _tagBigram2[j][i] + output2[idx][0][i] + maxscore2s[idx - 1][j];
            if (curscore > maxscore) {
              maxlastlabel = j;
              maxscore = curscore;
            }
          }
          maxscore2s[idx][i] = maxscore;
          maxlastlabel2s[idx][i] = maxlastlabel;
        }
        if (result1s[idx] == _label1_o && i != _label2_o)
          maxscore2s[idx][i] = -1e+20;
      }
    }

    result2s.resize(seq_size);
    maxFinalScore = maxscore2s[seq_size - 1][0];
    result2s[seq_size - 1] = 0;
    for (int i = 1; i < _label2Size; ++i) {
      if (maxscore2s[seq_size - 1][i] > maxFinalScore) {
        maxFinalScore = maxscore2s[seq_size - 1][i];
        result2s[seq_size - 1] = i;
      }
    }

    for (int idx = seq_size - 2; idx >= 0; idx--) {
      result2s[idx] = maxlastlabel2s[idx + 1][result2s[idx + 1]];
    }

    //release
    for (int idx = 0; idx < seq_size; idx++) {
      FreeSpace(&(output1[idx]));
      FreeSpace(&(output2[idx]));
    }
  }

  double computeScore(const Example& example) {
    int seq_size = example.m_features.size();

    Tensor<xpu, 2, double> output1[seq_size];
    Tensor<xpu, 2, double> output2[seq_size];

    //initialize
    for (int idx = 0; idx < seq_size; idx++) {
      output1[idx] = NewTensor<xpu>(Shape2(1, _label1Size), 0.0);
      output2[idx] = NewTensor<xpu>(Shape2(1, _label2Size), 0.0);
    }

    //forward propagation
    for (int idx = 0; idx < seq_size; idx++) {
      const Feature& feature = example.m_features[idx];
      _layer_linear2.ComputeForwardScore(feature.linear_features, output2[idx]);
    }

    double cost = 0.0;
    // viterbi algorithm
    NRVec<int> goldlabel1s(seq_size);
    goldlabel1s = -1;
    for (int idx = 0; idx < seq_size; idx++) {
      for (int i = 0; i < _label1Size; ++i) {
        if (example.m_label1s[idx][i] == 1) {
          goldlabel1s[idx] = i;
        }
      }
    }

    NRMat<double> maxscore1s(seq_size, _label1Size);
    NRMat<int> maxlastlabel1s(seq_size, _label1Size);
    double gold1Score = 0.0;
    for (int idx = 0; idx < seq_size; idx++) {
      if (idx == 0)
        gold1Score = output1[idx][0][goldlabel1s[idx]];
      else
        gold1Score += output1[idx][0][goldlabel1s[idx]] + _tagBigram1[goldlabel1s[idx - 1]][goldlabel1s[idx]];
      double delta = 1.0;
      for (int i = 0; i < _label1Size; ++i) {
        // can be changed with probabilities in future work
        if (idx == 0) {
          maxscore1s[idx][i] = output1[idx][0][i];
          if (goldlabel1s[idx] != i)
            maxscore1s[idx][i] = maxscore1s[idx][i] + delta;
          maxlastlabel1s[idx][i] = -1;
        } else {
          int maxlastlabel = 0;
          double maxscore = _tagBigram1[0][i] + output1[idx][0][i] + maxscore1s[idx - 1][0];
          for (int j = 1; j < _label1Size; ++j) {
            double curscore = _tagBigram1[j][i] + output1[idx][0][i] + maxscore1s[idx - 1][j];
            if (curscore > maxscore) {
              maxlastlabel = j;
              maxscore = curscore;
            }
          }
          maxscore1s[idx][i] = maxscore;
          if (goldlabel1s[idx] != i)
            maxscore1s[idx][i] = maxscore1s[idx][i] + delta;
          maxlastlabel1s[idx][i] = maxlastlabel;

        }
      }
    }

    NRVec<int> optLabel1s(seq_size);
    optLabel1s = 0;
    double max1Score = maxscore1s[seq_size - 1][0];
    for (int i = 1; i < _label1Size; ++i) {
      if (maxscore1s[seq_size - 1][i] > max1Score) {
        max1Score = maxscore1s[seq_size - 1][i];
        optLabel1s[seq_size - 1] = i;
      }
    }

    for (int idx = seq_size - 2; idx >= 0; idx--) {
      optLabel1s[idx] = maxlastlabel1s[idx + 1][optLabel1s[idx + 1]];
    }

    bool bCorrect = true;
    for (int idx = 0; idx < seq_size; idx++) {
      if (goldlabel1s[idx] == -1)
        continue;
      _eval1.overall_label_count++;
      if (optLabel1s[idx] == goldlabel1s[idx]) {
        _eval1.correct_label_count++;
      } else {
        bCorrect = false;
      }
    }

    double cur1cost = bCorrect ? 0.0 : max1Score - gold1Score;
    cost += cur1cost;

    // viterbi algorithm
    NRVec<int> goldlabel2s(seq_size);
    goldlabel2s = -1;
    for (int idx = 0; idx < seq_size; idx++) {
      for (int i = 0; i < _label2Size; ++i) {
        if (example.m_label2s[idx][i] == 1) {
          goldlabel2s[idx] = i;
        }
      }
    }

    NRMat<double> maxscore2s(seq_size, _label2Size);
    NRMat<int> maxlastlabel2s(seq_size, _label2Size);
    double gold2Score = 0.0;
    for (int idx = 0; idx < seq_size; idx++) {
      if (idx == 0)
        gold2Score = output2[idx][0][goldlabel2s[idx]];
      else
        gold2Score += output2[idx][0][goldlabel2s[idx]] + _tagBigram2[goldlabel2s[idx - 1]][goldlabel2s[idx]];
      double delta = 1.0;
      for (int i = 0; i < _label2Size; ++i) {
        // can be changed with probabilities in future work
        if (idx == 0) {
          maxscore2s[idx][i] = output2[idx][0][i];
          if (goldlabel2s[idx] != i)
            maxscore2s[idx][i] = maxscore2s[idx][i] + delta;
          maxlastlabel2s[idx][i] = -1;
        } else {
          int maxlastlabel = 0;
          double maxscore = _tagBigram2[0][i] + output2[idx][0][i] + maxscore2s[idx - 1][0];
          for (int j = 1; j < _label2Size; ++j) {
            double curscore = _tagBigram2[j][i] + output2[idx][0][i] + maxscore2s[idx - 1][j];
            if (curscore > maxscore) {
              maxlastlabel = j;
              maxscore = curscore;
            }
          }
          maxscore2s[idx][i] = maxscore;
          if (goldlabel2s[idx] != i)
            maxscore2s[idx][i] = maxscore2s[idx][i] + delta;
          maxlastlabel2s[idx][i] = maxlastlabel;
        }
        if (optLabel1s[idx] == _label1_o && i != _label2_o)
          maxscore2s[idx][i] = -1e+20;
      }

    }

    NRVec<int> optLabel2s(seq_size);
    optLabel2s = 0;
    double max2Score = maxscore2s[seq_size - 1][0];
    for (int i = 1; i < _label2Size; ++i) {
      if (maxscore2s[seq_size - 1][i] > max2Score) {
        max2Score = maxscore2s[seq_size - 1][i];
        optLabel2s[seq_size - 1] = i;
      }
    }

    for (int idx = seq_size - 2; idx >= 0; idx--) {
      optLabel2s[idx] = maxlastlabel2s[idx + 1][optLabel2s[idx + 1]];
    }

    bCorrect = true;
    for (int idx = 0; idx < seq_size; idx++) {
      if (goldlabel2s[idx] == -1)
        continue;
      _eval2.overall_label_count++;
      if (optLabel2s[idx] == goldlabel2s[idx]) {
        _eval2.correct_label_count++;
      } else {
        bCorrect = false;
      }
    }

    double cur2cost = bCorrect ? 0.0 : max2Score - gold2Score;
    cost += cur2cost;

    //release
    for (int idx = 0; idx < seq_size; idx++) {
      FreeSpace(&(output1[idx]));
      FreeSpace(&(output2[idx]));
    }
    return cost;
  }

  void updateParams(double nnRegular, double adaAlpha, double adaEps) {
    _layer_linear1.updateAdaGrad(nnRegular, adaAlpha, adaEps);
    _layer_linear2.updateAdaGrad(nnRegular, adaAlpha, adaEps);

    _grad_tagBigram1 = _grad_tagBigram1 + _tagBigram1 * nnRegular;
    _eg2_tagBigram1 = _eg2_tagBigram1 + _grad_tagBigram1 * _grad_tagBigram1;
    _tagBigram1 = _tagBigram1 - _grad_tagBigram1 * adaAlpha / F<nl_sqrt>(_eg2_tagBigram1 + adaEps);
    _grad_tagBigram1 = 0.0;

    _grad_tagBigram2 = _grad_tagBigram2 + _tagBigram2 * nnRegular;
    _eg2_tagBigram2 = _eg2_tagBigram2 + _grad_tagBigram2 * _grad_tagBigram2;
    _tagBigram2 = _tagBigram2 - _grad_tagBigram2 * adaAlpha / F<nl_sqrt>(_eg2_tagBigram2 + adaEps);
    _grad_tagBigram2 = 0.0;

  }

  void writeModel();

  void loadModel();

  void checkgrad(const vector<Example>& examples, Tensor<xpu, 2, double>& Wd, const Tensor<xpu, 2, double>& gradWd, const string& mark, int iter) {
    int charseed = mark.length();
    for (int i = 0; i < mark.length(); i++) {
      charseed = (int) (mark[i]) * 5 + charseed;
    }
    srand(iter + charseed);
    std::vector<int> idRows, idCols;
    idRows.clear();
    idCols.clear();
    for (int i = 0; i < Wd.size(0); ++i)
      idRows.push_back(i);
    for (int idx = 0; idx < Wd.size(1); idx++)
      idCols.push_back(idx);

    random_shuffle(idRows.begin(), idRows.end());
    random_shuffle(idCols.begin(), idCols.end());

    int check_i = idRows[0], check_j = idCols[0];

    double orginValue = Wd[check_i][check_j];

    Wd[check_i][check_j] = orginValue + 0.001;
    double lossAdd = 0.0;
    for (int i = 0; i < examples.size(); i++) {
      Example oneExam = examples[i];
      lossAdd += computeScore(oneExam);
    }

    Wd[check_i][check_j] = orginValue - 0.001;
    double lossPlus = 0.0;
    for (int i = 0; i < examples.size(); i++) {
      Example oneExam = examples[i];
      lossPlus += computeScore(oneExam);
    }

    double mockGrad = (lossAdd - lossPlus) / 0.002;
    mockGrad = mockGrad / examples.size();
    double computeGrad = gradWd[check_i][check_j];

    printf("Iteration %d, Checking gradient for %s[%d][%d]:\t", iter, mark.c_str(), check_i, check_j);
    printf("mock grad = %.18f, computed grad = %.18f\n", mockGrad, computeGrad);

    Wd[check_i][check_j] = orginValue;
  }

  void checkgrad(const vector<Example>& examples, Tensor<xpu, 2, double>& Wd, const Tensor<xpu, 2, double>& gradWd, const string& mark, int iter,
      const hash_set<int>& indexes) {
    int charseed = mark.length();
    for (int i = 0; i < mark.length(); i++) {
      charseed = (int) (mark[i]) * 5 + charseed;
    }
    srand(iter + charseed);
    std::vector<int> idRows, idCols;
    idRows.clear();
    idCols.clear();
    static hash_set<int>::iterator it;
    for (it = indexes.begin(); it != indexes.end(); ++it)
      idRows.push_back(*it);
    for (int idx = 0; idx < Wd.size(1); idx++)
      idCols.push_back(idx);

    random_shuffle(idRows.begin(), idRows.end());
    random_shuffle(idCols.begin(), idCols.end());

    int check_i = idRows[0], check_j = idCols[0];

    double orginValue = Wd[check_i][check_j];

    Wd[check_i][check_j] = orginValue + 0.001;
    double lossAdd = 0.0;
    for (int i = 0; i < examples.size(); i++) {
      Example oneExam = examples[i];
      lossAdd += computeScore(oneExam);
    }

    Wd[check_i][check_j] = orginValue - 0.001;
    double lossPlus = 0.0;
    for (int i = 0; i < examples.size(); i++) {
      Example oneExam = examples[i];
      lossPlus += computeScore(oneExam);
    }

    double mockGrad = (lossAdd - lossPlus) / 0.002;
    mockGrad = mockGrad / examples.size();
    double computeGrad = gradWd[check_i][check_j];

    printf("Iteration %d, Checking gradient for %s[%d][%d]:\t", iter, mark.c_str(), check_i, check_j);
    printf("mock grad = %.18f, computed grad = %.18f\n", mockGrad, computeGrad);

    Wd[check_i][check_j] = orginValue;

  }

  void checkgrads(const vector<Example>& examples, int iter) {
    checkgrad(examples, _layer_linear2._W, _layer_linear2._gradW, "_layer_linear2._W", iter);
    checkgrad(examples, _layer_linear2._b, _layer_linear2._gradb, "_layer_linear2._b", iter);
  }

public:
  inline void resetEval() {
    _eval2.reset();
  }

  inline void setDropValue(double dropOut) {
    _dropOut = dropOut;
  }

  inline void setLabelO(int label1_o, int label2_o)
  {
    _label1_o = label1_o; _label2_o = label2_o;
  }

};

#endif /* SRC_MultiDiscreteCRFMMClassifier_H_ */